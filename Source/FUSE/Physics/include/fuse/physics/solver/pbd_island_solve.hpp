#pragma once

#include <fuse/physics/physics_data.hpp>
#include <fuse/physics/solver/contact_island_graph.hpp>
#include <fuse/physics/solver/distance_constraint.hpp>
#include <fuse/physics/solver/solver_work_buffers.hpp>
#include <fuse/types.hpp>

#include <functional>
#include <vector>

namespace fuse::physics {

struct SolverParams;

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


/// Orphaned constraint index counts for one island (B4.4 deepen).
struct IslandConstraintIndexPreflight {
    u32 ownedContactCount = 0;
    u32 ownedDistanceCount = 0;
    u32 orphanedContactIndexCount = 0;
    u32 orphanedDistanceIndexCount = 0;
    bool skipped = false;

    bool has_orphans() const {
        return orphanedContactIndexCount > 0u || orphanedDistanceIndexCount > 0u;
    }

    bool can_solve() const { return !skipped && !has_orphans(); }
};

/// Sleep/mobility diagnostics for one island (B4.4 deepen).
struct IslandSleepPreflight {
    u32 bodyCount = 0;
    u32 sleepingBodyCount = 0;
    u32 staticBodyCount = 0;
    u32 movableBodyCount = 0;
    bool allSleeping = false;
    bool noMovableBodies = false;
    bool skipped = false;

    bool can_solve() const { return !skipped && !allSleeping && movableBodyCount > 0u; }
};

/// Wake-on-impulse preflight for one body (B4.4 deepen).
struct WakeOnImpulsePreflight {
    f32 forceMagnitudeSq = 0.f;
    f32 torqueMagnitudeSq = 0.f;
    bool sleeping = false;
    bool shouldWake = false;
    bool skipped = false;

    bool can_wake() const { return !skipped && shouldWake; }
};

/// Body-partition diagnostics across all islands (B4.4 deepen).
struct IslandBodyPartitionPreflight {
    u32 totalIslands = 0;
    u32 totalBodySlots = 0;
    u32 duplicateBodyCount = 0;
    bool partitionValid = true;
    bool skipped = false;

    bool can_dispatch() const { return !skipped && partitionValid; }
};

/// Constraint-iteration preflight for solver params (B4.4 deepen).
struct ConstraintIterationPreflight {
    u32 iterations = 0;
    f32 residualTolerance = 0.f;
    bool zeroIterations = false;
    bool skipped = false;

    bool can_iterate() const { return !skipped && !zeroIterations; }
};

/// Combined dispatch preflight (graph + timestep guard).
struct IslandDispatchPreflight {
    IslandSolvePreflight solve{};
    bool invalidDt = false;

    bool can_dispatch() const { return !skipped && !invalidDt && solve.can_dispatch(); }

/// Preflight island dispatch from pre-extracted jobs (parallel batch guard).
struct IslandDispatchJobPreflight {
    IslandSolveStats stats{};

    bool can_dispatch() const { return !skipped && !invalidDt && stats.dispatchableCount > 0u; }
    bool nonFiniteDt = false;
    bool skipped = false;

    bool can_dispatch() const { return !skipped && !invalidDt && !nonFiniteDt && solve.can_dispatch(); }
};

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

/// Per-island wake outcome (skip vs activate) for parallel batch stubs.
struct IslandWakeResult {
    bool woke = false;
    bool skipped = false;
    u32 islandIndex = ContactIslandGraph::invalidIsland;
};

/// Batch wake summary for parallel iteration stubs.
struct IslandBatchWakeResult {
    u32 wokeCount = 0;
    u32 skippedCount = 0;
    u32 wakeableCount = 0;
    bool skipped = false;

    bool any_woke() const { return wokeCount > 0u; }
};

/// Aggregate constraint-solve counts for graph-level batch guards.
struct IslandConstraintSolveGraphStats {
    u32 totalIslands = 0;
    u32 solveableCount = 0;
    u32 emptyCount = 0;
    u32 staleRefsCount = 0;
    u32 allSleepingCount = 0;
    u32 noMovableCount = 0;
};

/// Graph-level constraint-solve preflight for selective per-island solve dispatch.
struct IslandConstraintSolveGraphPreflight {
    IslandConstraintSolveGraphStats stats{};
    bool skipped = false;

    bool has_solveable_islands() const { return !skipped && stats.solveableCount > 0u; }
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

/// Wake-candidate diagnostics for sleeping bodies above velocity thresholds (B4.4 deepen).
struct WakeCandidateStats {
    u32 wakeCandidateCount = 0;
    u32 restingSleepingCount = 0;

/// Preflight for bodies that would wake on the next sleep pass evaluation.
struct WakePreflight {
    WakeCandidateStats stats{};

    bool has_wake_candidates() const { return !skipped && stats.wakeCandidateCount > 0u; }

/// Work-buffer capacity preflight before island constraint solve (B4.4 deepen).
struct IslandSolveWorkPreflight {
    u32 bodyCount = 0;
    u32 bufferCapacity = 0;
    bool insufficientBufferCapacity = false;

    bool can_solve() const { return !skipped && !insufficientBufferCapacity; }

/// Per-island constraint index and body-reference preflight (B4.4 deepen).
struct IslandConstraintIndexPreflight {
    u32 ownedContactCount = 0;
    u32 ownedDistanceCount = 0;
    u32 oobContactIndexCount = 0;
    u32 oobDistanceIndexCount = 0;
    u32 oobBodyRefCount = 0;

    bool indices_valid() const {
        return oobContactIndexCount == 0u && oobDistanceIndexCount == 0u && oobBodyRefCount == 0u;
    }

/// Aggregate contact-impulse warm-start counts for graph-level batch guards.
struct IslandContactImpulseWarmStartStats {
    u32 totalIslands = 0;
    u32 warmStartableCount = 0;
    u32 emptyCount = 0;
    u32 noImpulseCount = 0;

/// Graph-level contact-impulse warm-start preflight for selective per-island seeding.
struct IslandContactImpulseWarmStartGraphPreflight {
    IslandContactImpulseWarmStartStats stats{};
    bool invalidDt = false;

    bool can_warm_start() const { return !skipped && !invalidDt && stats.warmStartableCount > 0u; }
/// Per-island constraint solve preflight (OOB indices, immovable pairs) (B4.4 deepen).
struct IslandSolveJobPreflight {
    u32 contactCount = 0;
    u32 distanceCount = 0;
    u32 outOfRangeContacts = 0;
    u32 outOfRangeDistances = 0;
    u32 immovableConstraintPairs = 0;
    u32 solvableConstraintPairs = 0;

    bool can_solve() const { return !skipped && solvableConstraintPairs > 0u; }

/// Per-island body sleep state summary (B4.4 deepen).
struct IslandSleepPreflight {
    u32 dynamicBodyCount = 0;
    u32 sleepingBodyCount = 0;
    u32 staticBodyCount = 0;
    u32 bodiesWithForces = 0;
    u32 bodiesWithCcd = 0;

    bool all_dynamic_sleeping() const {
        return dynamicBodyCount > 0u && sleepingBodyCount == dynamicBodyCount;

    bool all_static() const {
        return dynamicBodyCount == 0u && staticBodyCount > 0u;

/// Combined sleep/wake preflight for one island (B4.4 deepen).
struct IslandSleepWakePreflight {
    IslandSleepPreflight sleep{};
    u32 activeContactCount = 0;
    u32 contactsTouchingSleepingBody = 0;
    bool should_wake = false;
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

/// Per-island sleep/wake diagnostics for solve dispatch (B4.4 deepen).
struct IslandSleepPreflight {
    u32 bodyCount = 0;
    u32 sleepingCount = 0;
    u32 staticCount = 0;
    u32 kinematicCount = 0;
    u32 awakeDynamicCount = 0;

    bool all_sleeping() const {
        return !skipped && bodyCount > 0u && sleepingCount == bodyCount;
    }

    bool can_solve() const { return !skipped && awakeDynamicCount > 0u; }
};

/// Wake candidates for sleeping bodies in one island (B4.4 deepen).
struct IslandWakePreflight {
    u32 awakeNeighborCount = 0;
    u32 externalForceCount = 0;
    u32 contactWakeCount = 0;

    bool should_wake() const {
        return !skipped && (awakeNeighborCount > 0u || externalForceCount > 0u || contactWakeCount > 0u);

/// Aggregate sleep counts for graph-level solve guards.
struct IslandSleepStats {
    u32 totalIslands = 0;
    u32 solvableCount = 0;
    bool nonFiniteDt = false;

    bool can_dispatch() const {
        return !skipped && !invalidDt && !nonFiniteDt && constraintCount > 0u;

/// Per-island sleep preflight for solve skip guards (B4.4 deepen).
    u32 awakeCount = 0;
    u32 outOfRangeCount = 0;
    bool allSleeping = false;

    bool can_solve() const { return !skipped && !allSleeping; }

/// Aggregate sleep counts for graph-level solve guards (B4.4 deepen).
struct IslandSleepGraphStats {
    u32 solveableCount = 0;
    u32 allSleepingCount = 0;
    u32 emptyCount = 0;

/// Combined job preflight including sleep guard (B4.4 deepen).
struct IslandSolveJobSleepPreflight {
    IslandSolveJobPreflight job{};
    IslandSleepPreflight sleep{};

    bool can_dispatch() const { return !skipped && job.can_dispatch() && sleep.can_solve(); }

/// Combined dispatch preflight including sleep guard (B4.4 deepen).
struct IslandSleepDispatchPreflight {
    IslandDispatchPreflight dispatch{};
    u32 solvableIslandCount = 0;
    u32 allSleepingIslandCount = 0;

    bool can_dispatch() const { return !skipped && dispatch.can_dispatch() && solvableIslandCount > 0u; }
/// Graph-level sleep preflight for batch solve guards (B4.4 deepen).
struct IslandSleepGraphPreflight {
    IslandSleepGraphStats stats{};

    bool can_dispatch() const { return !skipped && stats.solveableCount > 0u; }

/// Per-island wake preflight for sleep/wake pre-step guards (B4.4 deepen).
    u32 candidateCount = 0;
    u32 wakeCandidateCount = 0;
    u32 alreadyAwakeCount = 0;

    bool needs_wake_check() const { return !skipped && sleepingCount > 0u; }
    bool can_skip_wake_check() const { return skipped || sleepingCount == 0u; }

/// Body index coverage for one island solve pass (out-of-range body guards).
struct IslandBodyRefsPreflight {
    u32 ownedBodyCount = 0;
    u32 inRangeBodyCount = 0;
    u32 outOfRangeBodyCount = 0;

    bool can_solve() const {
        return !skipped && inRangeBodyCount > 0u && outOfRangeBodyCount == 0u;

/// Per-island sleep/wake diagnostics for selective solve dispatch (B4.4 deepen follow-up).
    u32 staticOrKinematicCount = 0;
    u32 activeCount = 0;


/// Per-island wake diagnostics for force-driven sleep exit (B4.4 deepen follow-up).
    u32 sleepingBodyCount = 0;
    u32 forcedWakeCount = 0;

    bool can_wake() const { return !skipped && forcedWakeCount > 0u; }

/// Input validation for island graph construction (B4.4 deepen follow-up).
struct IslandBuildPreflight {
    u32 validContactCount = 0;
    u32 invalidContactCount = 0;
    u32 validDistanceCount = 0;
    u32 invalidDistanceCount = 0;

    bool can_build() const {
        return !skipped && bodyCount > 0u &&
               (validContactCount > 0u || validDistanceCount > 0u || bodyCount > 0u);

/// Per-island sleep state for solve dispatch guards (B4.4 deepen follow-up).
    u32 dynamicBodyCount = 0;
    u32 awakeBodyCount = 0;


/// Aggregate sleep counts for graph-level batch guards (B4.4 deepen follow-up).
    u32 partiallyAwakeCount = 0;

/// Graph-level sleep preflight for selective per-island solve skipping.
    IslandSleepStats stats{};

    bool has_awake_islands() const {
        return !skipped && stats.partiallyAwakeCount > 0u;

/// Per-island wake candidate diagnostics (B4.4 deepen follow-up).
    bool needsWake = false;

/// Aggregate wake counts for graph-level batch guards (B4.4 deepen follow-up).
struct IslandWakeStats {
    u32 noWakeCount = 0;

/// Graph-level wake preflight for selective body wake dispatch.
struct IslandWakeGraphPreflight {
    IslandWakeStats stats{};

    bool has_wake_candidates() const { return !skipped && stats.wakeCandidateCount > 0u; }

/// Per-island awake vs immobile body counts for solve skip guards (B4.4 deepen).
struct IslandSolveBodiesPreflight {
    u32 immobileCount = 0;


/// Per-island sleep state for selective solve early-out (B4.4 deepen).

    bool can_skip_solve() const { return !skipped && allSleeping; }

/// Per-island wake hint when sleeping and awake dynamics coexist (B4.4 deepen).
    u32 sleepingDynamicCount = 0;

    bool should_wake() const { return !skipped && needsWake; }

/// Aggregate sleep counts for graph-level batch guards (B4.4 deepen).

    bool has_awake_islands() const { return !skipped && awakeCount > 0u; }
/// Per-island sleep/wake state for solve and sleep-detection preflights (B4.4 deepen).
struct IslandSleepWakePreflight {
    u32 totalBodies = 0;
    bool allDynamicSleeping = false;

    bool can_sleep() const {
        return !skipped && awakeDynamicCount == 0u && sleepingCount > 0u;

/// Aggregate sleep/wake counts for graph-level solve guards.
struct IslandSleepWakeStats {

/// Graph-level sleep/wake preflight for selective per-island solve dispatch.
struct IslandSleepWakeGraphPreflight {
    IslandSleepWakeStats stats{};

    bool can_solve() const { return !skipped && stats.solvableCount > 0u; }
};

/// Constraint index coverage for one island solve pass (stale/out-of-range guards).
struct IslandConstraintRefsPreflight {
    u32 inRangeContactCount = 0;
    u32 inRangeDistanceCount = 0;
    u32 validContactCount = 0;
    u32 solvableContactCount = 0;
    u32 solvableDistanceCount = 0;
    bool skipped = false;

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

/// Combined per-job solve preflight (job + constraint refs + sleep guards).
struct IslandSolveCombinedPreflight {
    IslandSolveJobPreflight job{};
    IslandConstraintRefsPreflight constraintRefs{};
    IslandSleepPreflight sleep{};
    bool skipped = false;

    bool can_dispatch() const {
        return !skipped && job.can_dispatch() && constraintRefs.can_solve() && sleep.can_solve();
    }

    bool has_solvable_constraints() const {
        return !skipped && (solvableContactCount > 0u || solvableDistanceCount > 0u);
    }
};

/// Per-island sleep/wake preflight for selective solve early-out (B4.4 deepen).
struct IslandSleepWakePreflight {
    u32 ownedBodyCount = 0;
    u32 sleepingCount = 0;
    u32 inactiveCount = 0;
    u32 activeDynamicCount = 0;
    u32 outOfRangeBodyCount = 0;
    bool skipped = false;
    bool allSleeping = false;
    bool allInactive = false;

    bool can_solve() const { return !skipped && activeDynamicCount > 0u; }
};

/// Aggregate sleep/wake counts for graph-level solve guards.
struct IslandSleepWakeStats {
    u32 totalIslands = 0;
    u32 activeCount = 0;
    u32 allInactiveCount = 0;
    u32 allSleepingCount = 0;
    u32 emptyCount = 0;
};

/// Graph-level sleep/wake preflight for selective per-island solve dispatch.
struct IslandSleepWakeGraphPreflight {
    IslandSleepWakeStats stats{};
    bool skipped = false;

    bool can_dispatch() const { return !skipped && stats.activeCount > 0u; }
};

/// Combined constraint-ref and sleep/wake preflight for one island solve pass.
struct IslandSolveBodiesPreflight {
    IslandConstraintRefsPreflight constraintRefs{};
    IslandSleepWakePreflight sleepWake{};
    bool skipped = false;

    bool can_solve() const {
        return !skipped && sleepWake.can_solve() && constraintRefs.has_solvable_constraints();
    }
};

/// Combined dispatch preflight (graph + timestep + sleep/wake guards).
struct IslandDispatchBodiesPreflight {
    IslandDispatchPreflight dispatch{};
    IslandSleepWakeGraphPreflight sleepWake{};
    bool skipped = false;

    bool can_dispatch() const {
        return !skipped && dispatch.can_dispatch() && sleepWake.can_dispatch();
    }
};

/// Combined constraint solve preflight (refs + bodies + sleep + dt guards).
struct IslandConstraintSolvePreflight {
    IslandConstraintRefsPreflight refs{};
    IslandBodyRefsPreflight bodies{};
    IslandSleepPreflight sleep{};
    bool invalidDt = false;
    bool skipped = false;

    bool can_solve() const {
        return !skipped && !invalidDt && refs.can_solve() && bodies.can_solve() && sleep.can_solve();
    }
};

/// Per-island body state scan for sleep/wake solve guards (B4.4 deepen follow-up pass).
struct IslandSleepWakePreflight {
    u32 bodyCount = 0;
    u32 inRangeBodyCount = 0;
    u32 sleepingCount = 0;
    u32 awakeDynamicCount = 0;
    u32 staticOrKinematicCount = 0;

    bool all_sleeping() const {
        return !skipped && inRangeBodyCount > 0u && sleepingCount == inRangeBodyCount;

    bool all_static_or_sleeping() const {
        return !skipped && inRangeBodyCount > 0u &&
               (staticOrKinematicCount + sleepingCount) == inRangeBodyCount;

    bool has_awake_dynamic() const { return awakeDynamicCount > 0u; }

    bool can_solve() const { return !skipped && has_awake_dynamic(); }

    bool should_remain_asleep() const { return all_sleeping(); }

    bool should_wake() const { return has_awake_dynamic(); }

/// Combined constraint-ref + sleep/wake preflight for one island solve pass.
    IslandSleepWakePreflight sleepWake{};

    bool can_solve() const { return !skipped && refs.can_solve() && sleepWake.can_solve(); }
/// Per-island sleep/wake coverage for constraint solve dispatch (B4.4 deepen).
    u32 wakeableCount = 0;

    bool is_all_sleeping() const { return bodyCount > 0u && sleepingCount == bodyCount; }
    bool has_wakeable_body() const { return wakeableCount > 0u; }

    bool can_solve() const { return !skipped && has_wakeable_body(); }

/// Combined constraint-ref and sleep/wake preflight for one island solve pass.


/// Aggregate sleep/wake counts for graph-level solve guards.
struct IslandSleepWakeStats {
    u32 totalIslands = 0;
    u32 solvableCount = 0;
    u32 allSleepingCount = 0;
    u32 emptyCount = 0;
    u32 inactiveCount = 0;

/// Graph-level sleep/wake preflight for selective per-island solve dispatch.
struct IslandSleepWakeGraphPreflight {
    IslandSleepWakeStats stats{};

    bool can_solve() const { return !skipped && stats.solvableCount > 0u; }
/// Body index coverage for one island solve pass (stale/out-of-range guards).
struct IslandBodyRefsPreflight {
    u32 ownedBodyCount = 0;
    u32 dynamicBodyCount = 0;

        return !skipped && ownedBodyCount > 0u && inRangeBodyCount == ownedBodyCount;

/// Combined constraint + body coverage for one island solve pass.
struct IslandSolveRefsPreflight {
    IslandConstraintRefsPreflight constraints{};

    bool can_solve() const { return !skipped && constraints.can_solve() && bodies.can_solve(); }

/// Per-island sleep aggregation for selective solve skip (B4.4 deepen follow-up).
struct IslandSleepPreflight {
    u32 dynamicAwakeCount = 0;

    bool all_dynamic_sleeping() const {
        return !skipped && dynamicAwakeCount == 0u && ownedBodyCount > staticOrKinematicCount;

    bool can_skip_solve() const { return all_dynamic_sleeping(); }

/// Per-island wake candidate scan for sleep/wake preflight (B4.4 deepen follow-up).
struct IslandWakePreflight {
    u32 wakeCandidateCount = 0;
    u32 externalForceCount = 0;
    u32 penetratingContactCount = 0;

    bool should_wake() const {
        return !skipped &&
               (wakeCandidateCount > 0u || externalForceCount > 0u || penetratingContactCount > 0u);

/// Graph-level sleep aggregation for batch solve skip guards.
struct IslandSleepGraphStats {
    u32 fullySleepingCount = 0;
    u32 partiallyAwakeCount = 0;

/// Graph-level sleep preflight for selective island solve skip.
struct IslandSleepGraphPreflight {
    IslandSleepGraphStats stats{};

    bool all_fully_sleeping() const {
        return !skipped && stats.fullySleepingCount == stats.totalIslands && stats.totalIslands > 0u;

    bool can_skip_all() const { return all_fully_sleeping(); }

/// Graph-level wake preflight for batch wake guards.
struct IslandWakeGraphPreflight {
    u32 wakeableIslandCount = 0;

    bool any_should_wake() const { return !skipped && wakeableIslandCount > 0u; }


/// Per-island sleep state for solve/wake guards (B4.4 deepen).

        return !skipped && awakeDynamicCount == 0u && sleepingCount > 0u;

    bool can_solve_awake() const { return !skipped && !all_dynamic_sleeping(); }

/// Per-island wake hint when mixed sleep/awake bodies share an island (B4.4 deepen).

    bool needs_wake() const { return !skipped && sleepingCount > 0u && awakeDynamicCount > 0u; }

/// Aggregate sleep counts for graph-level dispatch guards.
struct IslandSleepStats {
    u32 awakeCount = 0;

/// Graph-level sleep preflight for selective per-island solve dispatch.
    IslandSleepStats stats{};

    bool can_dispatch_awake() const { return !skipped && stats.awakeCount > 0u; }

/// Combined solve preflight (constraint refs + body refs + sleep).
struct IslandSolvePassPreflight {
    IslandConstraintRefsPreflight constraintRefs{};
    IslandBodyRefsPreflight bodyRefs{};

        return !skipped && constraintRefs.can_solve() && bodyRefs.can_solve() && sleep.can_solve_awake();
/// Per-island sleep state preflight for solve skip guards (B4.4 deepen follow-up).

    bool is_fully_sleeping() const {

    bool can_solve() const { return !skipped && !is_fully_sleeping(); }

/// Per-island wake preflight when contacts mix awake and sleeping bodies (B4.4 deepen follow-up).
    u32 ownedContactCount = 0;
    u32 awakeParticipantCount = 0;

    bool can_wake() const { return !skipped && wakeCandidateCount > 0u; }

/// Combined constraint-ref + sleep preflight for one island solve pass.
struct IslandSolveSleepPreflight {

    bool can_solve() const { return !skipped && refs.can_solve() && sleep.can_solve(); }

/// Graph-level sleep counts for batch solve guards.

/// Graph-level wake counts for batch wake guards.
struct IslandWakeStats {

    bool can_dispatch() const { return !skipped && stats.wakeableCount > 0u; }
/// Sleep/wake preflight for one island constraint solve pass (B4.4 deepen follow-up).
struct IslandSleepSolvePreflight {
    u32 sleepingBodyCount = 0;
    u32 awakeBodyCount = 0;

    bool can_solve() const { return !skipped && awakeBodyCount > 0u; }

/// Graph-level sleep preflight for island dispatch batching (B4.4 deepen follow-up).
    u32 sleepingOnlyCount = 0;

    bool can_dispatch() const { return !skipped && awakeCount > 0u; }

/// Wake preflight for sleeping islands with external activity (B4.4 deepen follow-up).
    u32 sleepingDynamicCount = 0;
    u32 nonZeroImpulseCount = 0;

        return !skipped && sleepingDynamicCount > 0u &&
               (awakeDynamicCount > 0u || nonZeroImpulseCount > 0u);

struct IslandSolveBodyPreflight {
    IslandSleepSolvePreflight sleep{};

};

/// Combined constraint-solve preflight (dt + refs + sleep/wake guards).
struct IslandConstraintSolvePreflight {
    IslandConstraintRefsPreflight refs{};
    IslandSleepWakePreflight sleepWake{};
    bool invalidDt = false;
    bool skipped = false;

    bool can_solve() const {
        return !skipped && !invalidDt && refs.can_solve() && sleepWake.can_solve();
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
    u32 inRangeContactBodyCount = 0;
    u32 outOfRangeContactBodyCount = 0;
    u32 inRangeDistanceBodyCount = 0;
    u32 outOfRangeDistanceBodyCount = 0;
    bool skipped = false;

    bool has_unsafe_body_refs() const {
        return outOfRangeContactBodyCount > 0u || outOfRangeDistanceBodyCount > 0u;
    }

    bool can_solve() const {
        return !skipped && !has_unsafe_body_refs() &&
               (inRangeContactCount > 0u || inRangeDistanceCount > 0u);
    }
};

/// Why island graph build would early-out (B4.5 deepen follow-up pass).
enum class IslandBuildRejectReason : u8 {
    None = 0,
    EmptyInput,
    OutOfRangeContactRefs,
    OutOfRangeDistanceRefs,
};

/// Human-readable label for island-build reject reasons (B4.5 deepen follow-up pass).
/// Human-readable label for island build reject reasons (B4.5 deepen follow-up pass).
const char* island_build_reject_reason_name(IslandBuildRejectReason reason);

/// Diagnose why island graph build would skip; vacuously succeeds when build may proceed (B4.5 deepen follow-up pass).
/// Why island graph build would early-out (B4.4 deepen follow-up pass).
    OutOfRangeRefs,

/// Human-readable label for island-build reject reasons (B4.4 deepen follow-up pass).

/// Diagnose why island graph build would skip; vacuously succeeds when build may proceed.
IslandBuildRejectReason island_build_reject_reason(
    u32 bodyCount,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints);

/// Returns true when `island_build_reject_reason` matches `expected` (B4.5 deepen follow-up pass).
/// Returns true when `island_build_reject_reason` matches `expected`.
bool island_build_rejects_for_reason(
    u32 bodyCount,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints,
    IslandBuildRejectReason expected);

/// Why island constraint solve would early-out (B4.5 deepen follow-up pass).
enum class IslandConstraintSolveRejectReason : u8 {
    None = 0,
    EmptyIsland,
    OutOfRangeIslandIndex,
    StaleConstraintRefs,
    NoMovableBodies,
};

/// Human-readable label for island constraint-solve reject reasons (B4.5 deepen follow-up pass).
const char* island_constraint_solve_reject_reason_name(IslandConstraintSolveRejectReason reason);

/// Diagnose why one island constraint solve would skip (B4.5 deepen follow-up pass).
/// Why per-island constraint solve would early-out (B4.4 deepen follow-up pass).
    NoInRangeRefs,

/// Human-readable label for island constraint-solve reject reasons (B4.4 deepen follow-up pass).

/// Diagnose why one island constraint solve would skip.
IslandConstraintSolveRejectReason island_constraint_solve_reject_reason(
    const ContactIslandGraph::Island& island,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints);

/// Diagnose why constraint solve by island index would skip (B4.5 deepen follow-up pass).
IslandConstraintSolveRejectReason island_constraint_solve_reject_reason_by_index(
    const ContactIslandGraph& graph,
    u32 islandIndex,

/// Returns true when `island_constraint_solve_reject_reason` matches `expected` (B4.5 deepen follow-up pass).
bool island_constraint_solve_rejects_for_reason(
    const std::vector<DistanceConstraint>& distanceConstraints,
    IslandConstraintSolveRejectReason expected);

/// Why island sleep solve would early-out (B4.5 deepen follow-up pass).
enum class IslandSleepRejectReason : u8 {
    AllSleeping,

/// Human-readable label for island sleep reject reasons (B4.5 deepen follow-up pass).
const char* island_sleep_reject_reason_name(IslandSleepRejectReason reason);

/// Diagnose why one island sleep-solve would skip (B4.5 deepen follow-up pass).
IslandSleepRejectReason island_sleep_reject_reason(const ContactIslandGraph::Island& island,
                                                   const RigidBodySoA& bodies);

/// Diagnose why sleep-solve by island index would skip (B4.5 deepen follow-up pass).
IslandSleepRejectReason island_sleep_reject_reason_by_index(const ContactIslandGraph& graph,

/// Returns true when `island_sleep_reject_reason` matches `expected` (B4.5 deepen follow-up pass).
bool island_sleep_rejects_for_reason(const ContactIslandGraph::Island& island,
                                     IslandSleepRejectReason expected);

/// Why graph-level island sleep batching would early-out (B4.5 deepen follow-up pass).
enum class IslandSleepGraphRejectReason : u8 {
    NoConstrainedIslands,
    AllIslandsSleeping,

/// Human-readable label for graph sleep reject reasons (B4.5 deepen follow-up pass).
const char* island_sleep_graph_reject_reason_name(IslandSleepGraphRejectReason reason);

/// Diagnose why graph-level sleep batching would skip (B4.5 deepen follow-up pass).
IslandSleepGraphRejectReason island_sleep_graph_reject_reason(const ContactIslandGraph& graph,

/// Returns true when `island_sleep_graph_reject_reason` matches `expected` (B4.5 deepen follow-up pass).
bool island_sleep_graph_rejects_for_reason(const ContactIslandGraph& graph,
                                           IslandSleepGraphRejectReason expected);

/// Why island wake would early-out (B4.5 deepen follow-up pass).
enum class IslandWakeRejectReason : u8 {
    NoSleepingBodies,
    NoActiveDynamic,

/// Human-readable label for island wake reject reasons (B4.5 deepen follow-up pass).
const char* island_wake_reject_reason_name(IslandWakeRejectReason reason);

/// Diagnose why one island wake would skip (B4.5 deepen follow-up pass).
IslandWakeRejectReason island_wake_reject_reason(const ContactIslandGraph::Island& island,

/// Diagnose why wake by island index would skip (B4.5 deepen follow-up pass).
IslandWakeRejectReason island_wake_reject_reason_by_index(const ContactIslandGraph& graph,

/// Returns true when `island_wake_reject_reason` matches `expected` (B4.5 deepen follow-up pass).
bool island_wake_rejects_for_reason(const ContactIslandGraph::Island& island,
                                    IslandWakeRejectReason expected);

/// Why graph-level island wake batching would early-out (B4.5 deepen follow-up pass).
enum class IslandWakeGraphRejectReason : u8 {
    NoWakeableIslands,

/// Human-readable label for graph wake reject reasons (B4.5 deepen follow-up pass).
const char* island_wake_graph_reject_reason_name(IslandWakeGraphRejectReason reason);

/// Diagnose why graph-level wake batching would skip (B4.5 deepen follow-up pass).
IslandWakeGraphRejectReason island_wake_graph_reject_reason(const ContactIslandGraph& graph,

/// Returns true when `island_wake_graph_reject_reason` matches `expected` (B4.5 deepen follow-up pass).
bool island_wake_graph_rejects_for_reason(const ContactIslandGraph& graph,
/// Returns true when `island_constraint_solve_reject_reason` matches `expected`.
    const ContactIslandGraph::Island& island,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,

/// Why per-island sleep solve would early-out (B4.4 deepen follow-up pass).
    None = 0,
    EmptyIsland,
};

/// Human-readable label for island sleep reject reasons (B4.4 deepen follow-up pass).

/// Diagnose why one island sleep preflight would skip solve.

/// Returns true when `island_sleep_reject_reason` matches `expected`.

/// Why per-island wake would early-out (B4.4 deepen follow-up pass).
    NoWakeTarget,

/// Human-readable label for island wake reject reasons (B4.4 deepen follow-up pass).

/// Diagnose why one island wake preflight would skip sleeper activation.

/// Returns true when `island_wake_reject_reason` matches `expected`.

/// Why graph-level sleep batching would early-out (B4.4 deepen follow-up pass).
    EmptyGraph,

/// Human-readable label for graph sleep reject reasons (B4.4 deepen follow-up pass).

/// Diagnose why graph-level sleep solve batch would skip.

/// Returns true when `island_sleep_graph_reject_reason` matches `expected`.

/// Why graph-level wake batching would early-out (B4.4 deepen follow-up pass).

/// Human-readable label for graph wake reject reasons (B4.4 deepen follow-up pass).

/// Diagnose why graph-level wake batch would skip.

/// Returns true when `island_wake_graph_reject_reason` matches `expected`.
                                          IslandWakeGraphRejectReason expected);

/// Input coverage for island graph build (out-of-range body-index guards).
struct IslandBuildStats {
    u32 bodyCount = 0;
    u32 contactSlotCount = 0;
    u32 distanceSlotCount = 0;
    u32 selfContactCount = 0;
    u32 validContactCount = 0;
    u32 inRangeContactCount = 0;
    u32 inRangeDistanceCount = 0;
    u32 selfPairContactCount = 0;
    u32 selfPairDistanceCount = 0;
    u32 outOfRangeContactBodyCount = 0;
    u32 outOfRangeDistanceBodyCount = 0;
    u32 selfContactCount = 0;
    u32 invalidContactCount = 0;
    u32 selfDistanceCount = 0;
};

/// Explicit outcome for guarded island graph build (B4.4 deepen).
struct IslandBuildResult {
    bool built = false;
    bool skipped = false;
    bool unsafeRefs = false;
    IslandBuildStats stats{};

/// Preflight diagnostics for island graph build inputs (B4.4 deepen).
struct IslandBuildPreflight {
    IslandBuildRejectReason reason = IslandBuildRejectReason::None;
    IslandBuildStats stats{};

    bool has_unsafe_refs() const {
        return stats.outOfRangeContactBodyCount > 0u || stats.outOfRangeDistanceBodyCount > 0u;

    bool has_degenerate_refs() const { return stats.selfContactCount > 0u; }

    bool can_build() const { return !skipped && !has_unsafe_refs() && !has_degenerate_refs(); }
    bool can_build() const { return reason == IslandBuildRejectReason::None && !skipped && !has_unsafe_refs(); }
    bool has_degenerate_refs() const {
        return stats.selfPairContactCount > 0u || stats.selfPairDistanceCount > 0u;
    }

    bool can_build() const { return reason == IslandBuildRejectReason::None; }
    bool can_build() const { return !skipped && reason == IslandBuildRejectReason::None && !has_unsafe_refs(); }
    bool can_build() const { return !skipped && reason == IslandBuildRejectReason::None; }
    bool has_degenerate_refs() const {
        return stats.selfContactCount > 0u || stats.selfDistanceCount > 0u;
    }

    bool can_build() const { return !skipped && !has_unsafe_refs() && !has_degenerate_refs(); }
};

/// Explicit outcome for guarded island graph build.
struct IslandBuildResult {
    bool built = false;
    bool skipped = false;
    bool unsafeRefs = false;
    bool degenerateRefs = false;
    IslandBuildStats stats{};
};

/// Guarded island graph build outcome (B4.4 deepen follow-up).
struct IslandBuildResult {
    bool built = false;
    bool skipped = false;
    IslandBuildRejectReason reason = IslandBuildRejectReason::None;
/// Why island graph build would reject (B4.4 deepen follow-up pass).
enum class IslandBuildRejectReason : u8 {
    None = 0,
    EmptyInputs,
    OutOfRangeContactBodies,
    OutOfRangeDistanceBodies,
};

/// Human-readable label for island build reject reasons (logging / tests).
const char* islandBuildRejectReasonName(IslandBuildRejectReason reason);

/// Diagnose why build would reject; vacuously succeeds when build may proceed.
IslandBuildRejectReason island_build_reject_reason(
    u32 bodyCount,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints);

/// Returns true when `island_build_reject_reason` matches `expected` (B4.4 deepen follow-up pass).
bool island_build_rejects_for_reason(
    const std::vector<DistanceConstraint>& distanceConstraints,
    IslandBuildRejectReason expected);

/// Read-only island build diagnostics with reject reason — no mutation (B4.4 deepen follow-up).
struct IslandBuildDeepenPreflight {
    IslandBuildStats stats{};
    bool rejected = false;

    bool can_build() const { return !rejected; }

/// Populate island build deepen preflight without mutating the graph (B4.4 deepen follow-up).
IslandBuildDeepenPreflight preflight_island_build_deepen(

/// Returns true when island graph build should be skipped (B4.4 deepen follow-up).
bool should_skip_island_build_deepen(

/// Non-mutating build predicate — inverse of `should_skip_island_build_deepen` (B4.4 deepen follow-up).
bool should_run_island_build(

/// Body participation for one island constraint solve (sleep/static/movable guards).
struct IslandSolveBodiesPreflight {
    u32 inRangeBodyCount = 0;
    u32 staticOrKinematicCount = 0;
    u32 sleepingCount = 0;
    u32 movableCount = 0;

    bool can_solve() const { return !skipped && movableCount > 0u; }

/// Why per-island constraint solve would early-out (B4.5 deepen follow-up pass).
enum class IslandConstraintSolveRejectReason : u8 {
    None = 0,
    EmptyIsland,
    StaleConstraintRefs,
    AllSleeping,
    NoMovableBodies,
};

/// Human-readable label for island constraint-solve reject reasons (B4.5 deepen follow-up pass).
const char* island_constraint_solve_reject_reason_name(IslandConstraintSolveRejectReason reason);

/// Diagnose why island constraint solve would skip; vacuously succeeds when solve may proceed (B4.5 deepen follow-up pass).
IslandConstraintSolveRejectReason island_constraint_solve_reject_reason(
    const ContactIslandGraph::Island& island,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints);

/// Returns true when `island_constraint_solve_reject_reason` matches `expected` (B4.5 deepen follow-up pass).
bool island_constraint_solve_rejects_for_reason(
    const ContactIslandGraph::Island& island,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints,
    IslandConstraintSolveRejectReason expected);

/// Combined constraint-ref + body participation preflight for one island solve pass.
struct IslandConstraintSolvePreflight {
    IslandConstraintSolveRejectReason reason = IslandConstraintSolveRejectReason::None;
    IslandConstraintRefsPreflight refs{};
    IslandSolveBodiesPreflight bodies{};

    bool can_solve() const { return !skipped && refs.can_solve() && bodies.can_solve(); }

/// Aggregate constraint-solve counts for graph-level batch guards.
struct IslandConstraintSolveGraphStats {
    u32 totalIslands = 0;
    u32 solveableCount = 0;
    u32 blockedByRefsCount = 0;
    u32 blockedByBodiesCount = 0;
    u32 allSleepingCount = 0;
    u32 emptyCount = 0;

/// Graph-level constraint-solve preflight for selective per-island solve dispatch.
struct IslandConstraintSolveGraphPreflight {
    IslandConstraintSolveGraphStats stats{};

    bool can_solve_any() const { return !skipped && stats.solveableCount > 0u; }

/// Per-island constraint solve outcome (skip vs solve) for guarded entry points.
struct IslandConstraintSolveResult {
    bool solved = false;
    bool skipped = false;
    u32 islandIndex = ContactIslandGraph::invalidIsland;
    bool can_solve() const { return reason == IslandConstraintSolveRejectReason::None; }
    bool can_solve() const { return !skipped && reason == IslandConstraintSolveRejectReason::None; }
};

/// Why one island constraint solve would reject (B4.4 deepen follow-up pass).
enum class IslandConstraintSolveRejectReason : u8 {
    None = 0,
    EmptyIsland,
    NoInRangeConstraints,
    NoMovableBodies,

/// Human-readable label for island constraint solve reject reasons (logging / tests).
const char* islandConstraintSolveRejectReasonName(IslandConstraintSolveRejectReason reason);

/// Diagnose why constraint solve would reject; vacuously succeeds when solve may proceed.
IslandConstraintSolveRejectReason island_constraint_solve_reject_reason(
    const ContactIslandGraph::Island& island,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints);

/// Returns true when `island_constraint_solve_reject_reason` matches `expected` (B4.4 deepen follow-up pass).
bool island_constraint_solve_rejects_for_reason(
    const std::vector<DistanceConstraint>& distanceConstraints,
    IslandConstraintSolveRejectReason expected);

/// Read-only constraint solve diagnostics with reject reason — no mutation (B4.4 deepen follow-up).
struct IslandConstraintSolveDeepenPreflight {
    IslandConstraintSolveRejectReason reason = IslandConstraintSolveRejectReason::None;
    IslandConstraintRefsPreflight refs{};
    IslandSolveBodiesPreflight bodies{};
    bool rejected = false;

    bool can_solve() const { return !rejected; }

/// Populate constraint solve deepen preflight without mutating bodies (B4.4 deepen follow-up).
IslandConstraintSolveDeepenPreflight preflight_island_constraint_solve_deepen(

/// Returns true when island constraint solve should be skipped (B4.4 deepen follow-up).
bool should_skip_island_constraint_solve_deepen(

/// Non-mutating constraint solve predicate — inverse of skip deepen guard (B4.4 deepen follow-up).
bool should_run_island_constraint_solve(

/// Why island solve dispatch would reject (B4.4 deepen follow-up pass).
enum class IslandDispatchRejectReason : u8 {
    NoDispatchableIslands,
    InvalidDt,
    NonFiniteDt,

/// Human-readable label for island dispatch reject reasons (logging / tests).
const char* islandDispatchRejectReasonName(IslandDispatchRejectReason reason);

/// Diagnose why graph-level island dispatch would reject.
IslandDispatchRejectReason island_dispatch_reject_reason(const ContactIslandGraph& graph, f32 dt);

/// Returns true when `island_dispatch_reject_reason` matches `expected` (B4.4 deepen follow-up pass).
bool island_dispatch_rejects_for_reason(const ContactIslandGraph& graph,
                                        f32 dt,
                                        IslandDispatchRejectReason expected);

/// Read-only dispatch diagnostics with reject reason — no mutation (B4.4 deepen follow-up).
struct IslandDispatchDeepenPreflight {
    IslandDispatchRejectReason reason = IslandDispatchRejectReason::None;
    IslandSolveStats stats{};

    bool can_dispatch() const { return !rejected; }

/// Populate dispatch deepen preflight without mutating bodies (B4.4 deepen follow-up).
IslandDispatchDeepenPreflight preflight_island_dispatch_deepen(const ContactIslandGraph& graph, f32 dt);

/// Returns true when graph-level island dispatch should be skipped (B4.4 deepen follow-up).
bool should_skip_island_dispatch_deepen(const ContactIslandGraph& graph, f32 dt);

/// Non-mutating dispatch predicate — inverse of `should_skip_island_dispatch_deepen` (B4.4 deepen follow-up).
bool should_run_island_dispatch(const ContactIslandGraph& graph, f32 dt);

/// Why one island solve job would reject (B4.4 deepen follow-up pass).
enum class IslandSolveJobRejectReason : u8 {
    OutOfRangeIndex,
    ZeroConstraints,

/// Human-readable label for island solve job reject reasons (logging / tests).
const char* islandSolveJobRejectReasonName(IslandSolveJobRejectReason reason);

/// Diagnose why one extracted solve job would reject.
IslandSolveJobRejectReason island_solve_job_reject_reason(const IslandSolveJob& job, f32 dt);

/// Returns true when `island_solve_job_reject_reason` matches `expected` (B4.4 deepen follow-up pass).
bool island_solve_job_rejects_for_reason(const IslandSolveJob& job,
                                           IslandSolveJobRejectReason expected);

/// Read-only solve-job diagnostics with reject reason — no mutation (B4.4 deepen follow-up).
struct IslandSolveJobDeepenPreflight {
    IslandSolveJobRejectReason reason = IslandSolveJobRejectReason::None;
    u32 constraintCount = 0;


/// Populate solve-job deepen preflight without mutating bodies (B4.4 deepen follow-up).
IslandSolveJobDeepenPreflight preflight_solve_island_job_deepen(const IslandSolveJob& job, f32 dt);

/// Returns true when solve job dispatch should be skipped (B4.4 deepen follow-up).
bool should_skip_solve_island_job_deepen(const IslandSolveJob& job, f32 dt);

/// Non-mutating solve-job predicate — inverse of skip deepen guard (B4.4 deepen follow-up).
bool should_run_solve_island_job(const IslandSolveJob& job, f32 dt);
/// Combined job dispatch + constraint solve preflight for one island (B4.4 deepen).
struct IslandConstraintSolveJobPreflight {
    IslandSolveJobPreflight job{};
    IslandConstraintSolvePreflight solve{};
    bool skipped = false;

    bool can_solve() const { return !skipped && job.can_dispatch() && solve.can_solve(); }
};

/// Per-island wake outcome for parallel batch stubs (B4.4 deepen).
struct IslandWakeResult {
    bool woke = false;
    u32 islandIndex = ContactIslandGraph::invalidIsland;
    u32 bodiesWoken = 0;

/// Batch wake summary for graph-level guards (B4.4 deepen).
struct IslandBatchWakeResult {
    u32 wokeCount = 0;
    u32 skippedCount = 0;
    u32 wakeableCount = 0;

    bool any_woke() const { return wokeCount > 0u; }

/// Batch dispatch with body-participation guards (B4.4 deepen).
struct IslandBatchBodiesDispatchResult {
    u32 solvedCount = 0;
    u32 solveableCount = 0;

    bool any_solved() const { return solvedCount > 0u; }
/// Aggregate constraint-solve participation counts for graph-level batch guards.
struct IslandConstraintSolveGraphStats {
    u32 totalIslands = 0;
    u32 allSleepingCount = 0;
    u32 staleRefsCount = 0;
    u32 emptyCount = 0;

/// Graph-level constraint solve preflight for selective per-island dispatch.
struct IslandConstraintSolveGraphPreflight {
    IslandConstraintSolveGraphStats stats{};

    bool can_dispatch() const { return !skipped && stats.solveableCount > 0u; }

/// Per-island sleep state for solve early-out stubs.
struct IslandSleepPreflight {
    IslandSleepRejectReason reason = IslandSleepRejectReason::None;
    bool can_solve() const {
        return !skipped && reason == IslandConstraintSolveRejectReason::None && refs.can_solve() &&
               bodies.can_solve();
    }

/// Why per-island sleep solve would early-out (B4.5 deepen follow-up pass).
enum class IslandSleepSolveRejectReason : u8 {
    AllSleeping,

/// Human-readable label for island sleep-solve reject reasons (B4.5 deepen follow-up pass).
const char* island_sleep_solve_reject_reason_name(IslandSleepSolveRejectReason reason);

/// Diagnose why island sleep solve would skip; vacuously succeeds when solve may proceed (B4.5 deepen follow-up pass).
IslandSleepSolveRejectReason island_sleep_solve_reject_reason(const ContactIslandGraph::Island& island,
                                                              const RigidBodySoA& bodies);

/// Returns true when `island_sleep_solve_reject_reason` matches `expected` (B4.5 deepen follow-up pass).
bool island_sleep_solve_rejects_for_reason(const ContactIslandGraph::Island& island,
                                           IslandSleepSolveRejectReason expected);

    IslandSleepSolveRejectReason reason = IslandSleepSolveRejectReason::None;
    u32 bodyCount = 0;
    u32 sleepingCount = 0;
    u32 staticOrKinematicCount = 0;
    u32 activeDynamicCount = 0;
    bool allSleeping = false;

    bool can_skip_solve() const { return !skipped && allSleeping; }

/// Why island sleep-solve would skip (B4.4 deepen follow-up pass).
enum class IslandSleepSolveRejectReason : u8 {
    None = 0,
    EmptyIsland,
    OutOfRangeIndex,
    AllSleeping,
    bool can_skip_solve() const { return reason == IslandSleepRejectReason::AllSleeping; }
    bool can_skip_solve() const {
        return !skipped && reason == IslandSleepRejectReason::AllSleeping;
    }
};

/// Human-readable label for island sleep-solve reject reasons (logging / tests).
const char* islandSleepSolveRejectReasonName(IslandSleepSolveRejectReason reason);

/// Diagnose why sleep-solve would skip one island; `None` means solve may proceed.
IslandSleepSolveRejectReason island_sleep_solve_reject_reason(const ContactIslandGraph::Island& island,
                                                              const RigidBodySoA& bodies);

/// Diagnose by island index; out-of-range indices return `OutOfRangeIndex`.
IslandSleepSolveRejectReason island_sleep_solve_reject_reason_by_index(const ContactIslandGraph& graph,
                                                                       u32 islandIndex,

/// Returns true when `island_sleep_solve_reject_reason` matches `expected` (B4.4 deepen follow-up pass).
bool island_sleep_solve_rejects_for_reason(const ContactIslandGraph::Island& island,
                                           const RigidBodySoA& bodies,
                                           IslandSleepSolveRejectReason expected);

/// Read-only sleep-solve diagnostics with reject reason — no mutation (B4.4 deepen follow-up).
struct IslandSleepSolveDeepenPreflight {
    IslandSleepSolveRejectReason reason = IslandSleepSolveRejectReason::None;
    u32 bodyCount = 0;
    u32 sleepingCount = 0;
    u32 activeDynamicCount = 0;
    bool rejected = false;

    bool can_solve() const { return !rejected; }

/// Populate sleep-solve deepen preflight without mutating bodies (B4.4 deepen follow-up).
IslandSleepSolveDeepenPreflight preflight_island_sleep_solve_deepen(const ContactIslandGraph::Island& island,

/// Populate sleep-solve deepen preflight by island index (B4.4 deepen follow-up).
IslandSleepSolveDeepenPreflight preflight_island_sleep_solve_deepen_by_index(const ContactIslandGraph& graph,

/// Returns true when sleep-solve should be skipped for one island (B4.4 deepen follow-up).
bool should_skip_island_sleep_solve_deepen(const ContactIslandGraph::Island& island,

/// Non-mutating sleep-solve predicate — inverse of skip deepen guard (B4.4 deepen follow-up).
bool should_run_island_sleep_solve(const ContactIslandGraph::Island& island, const RigidBodySoA& bodies);
    bool can_skip_solve() const {
        return !skipped && reason == IslandSleepSolveRejectReason::AllSleeping && allSleeping;
    }

/// Why per-island wake would early-out (B4.5 deepen follow-up pass).
enum class IslandWakeRejectReason : u8 {
    NoMixedSleepState,

/// Human-readable label for island wake reject reasons (B4.5 deepen follow-up pass).
const char* island_wake_reject_reason_name(IslandWakeRejectReason reason);

/// Diagnose why island wake would skip; vacuously succeeds when wake may proceed (B4.5 deepen follow-up pass).
IslandWakeRejectReason island_wake_reject_reason(const ContactIslandGraph::Island& island,

/// Returns true when `island_wake_reject_reason` matches `expected` (B4.5 deepen follow-up pass).
bool island_wake_rejects_for_reason(const ContactIslandGraph::Island& island,
                                    IslandWakeRejectReason expected);

/// Per-island wake hint when active dynamics neighbor sleeping bodies.
struct IslandWakePreflight {
    IslandWakeRejectReason reason = IslandWakeRejectReason::None;
    u32 bodyCount = 0;
    u32 sleepingCount = 0;
    u32 activeDynamicCount = 0;
    bool hasMixedSleepState = false;

    bool should_wake_sleepers() const {
        return !skipped && hasMixedSleepState && activeDynamicCount > 0u;

/// Wake + constraint-solve pipeline preflight for one island (B4.4 deepen follow-up).
struct IslandSolvePipelinePreflight {
    IslandWakePreflight wake{};
    IslandConstraintSolvePreflight solve{};
    IslandSleepPreflight sleep{};

    bool can_solve() const { return !skipped && solve.can_solve() && !sleep.can_skip_solve(); }
    bool should_wake_first() const { return !skipped && wake.should_wake_sleepers(); }

/// Why island sleeper wake would reject (B4.4 deepen follow-up pass).
enum class IslandWakeRejectReason : u8 {
    None = 0,
    EmptyIsland,
    OutOfRangeIndex,
    NoMixedSleepState,
    bool should_wake_sleepers() const { return reason == IslandWakeRejectReason::None && hasMixedSleepState; }
        return !skipped && reason == IslandWakeRejectReason::None && hasMixedSleepState &&
               activeDynamicCount > 0u;
    }
    bool should_wake_sleepers() const { return !skipped && reason == IslandWakeRejectReason::None; }
};

/// Combined dispatch preflight with constraint-solve and sleep guards (B4.5 deepen follow-up pass).
struct IslandDispatchDeepenPreflight {
    IslandDispatchPreflight dispatch{};
    IslandConstraintSolvePreflight constraintSolve{};
    bool skipped = false;

    bool can_dispatch() const {
        return !skipped && dispatch.can_dispatch() && constraintSolve.can_solve() && !sleep.can_skip_solve();
};

/// Human-readable label for island wake reject reasons (logging / tests).
const char* islandWakeRejectReasonName(IslandWakeRejectReason reason);

/// Diagnose why wake would reject one island; `None` means wake may proceed.
IslandWakeRejectReason island_wake_reject_reason(const ContactIslandGraph::Island& island,
                                                 const RigidBodySoA& bodies);

/// Diagnose by island index; out-of-range indices return `OutOfRangeIndex`.
IslandWakeRejectReason island_wake_reject_reason_by_index(const ContactIslandGraph& graph,
                                                          u32 islandIndex,

/// Returns true when `island_wake_reject_reason` matches `expected` (B4.4 deepen follow-up pass).
bool island_wake_rejects_for_reason(const ContactIslandGraph::Island& island,
                                    const RigidBodySoA& bodies,
                                    IslandWakeRejectReason expected);

/// Read-only wake diagnostics with reject reason — no mutation (B4.4 deepen follow-up).
struct IslandWakeDeepenPreflight {
    IslandWakeRejectReason reason = IslandWakeRejectReason::None;
    u32 sleepingCount = 0;
    u32 activeDynamicCount = 0;
    bool rejected = false;

    bool can_wake() const { return !rejected; }

/// Populate wake deepen preflight without mutating bodies (B4.4 deepen follow-up).
IslandWakeDeepenPreflight preflight_island_wake_deepen(const ContactIslandGraph::Island& island,

/// Populate wake deepen preflight by island index (B4.4 deepen follow-up).
IslandWakeDeepenPreflight preflight_island_wake_deepen_by_index(const ContactIslandGraph& graph,

/// Returns true when island wake should be skipped (B4.4 deepen follow-up).
bool should_skip_island_wake_deepen(const ContactIslandGraph::Island& island, const RigidBodySoA& bodies);

/// Non-mutating wake predicate — inverse of skip deepen guard (B4.4 deepen follow-up).
bool should_run_island_wake(const ContactIslandGraph::Island& island, const RigidBodySoA& bodies);
/// Combined sleep + wake hints for one island preflight.
struct IslandSleepWakePreflight {
    IslandSleepPreflight sleep{};
    IslandWakePreflight wake{};

    bool can_solve_after_wake() const {
        return !skipped && !sleep.can_skip_solve();
    }

    bool should_wake_before_solve() const { return !skipped && wake.should_wake_sleepers(); }

/// Per-island wake outcome for batch guards.
struct IslandWakeResult {
    bool woke = false;
    u32 islandIndex = ContactIslandGraph::invalidIsland;

/// Batch wake summary for graph-level guards.
struct IslandBatchWakeResult {
    u32 wokeCount = 0;
    u32 skippedCount = 0;
    u32 wakeableCount = 0;

    bool any_woke() const { return wokeCount > 0u; }

/// Aggregate sleep counts for graph-level batch guards.
struct IslandSleepGraphStats {
    u32 mixedSleepCount = 0;
    u32 fullyActiveCount = 0;

/// Graph-level sleep preflight for selective per-island solve skipping.
struct IslandSleepGraphPreflight {
    IslandSleepGraphRejectReason reason = IslandSleepGraphRejectReason::None;
    IslandSleepGraphStats stats{};

    bool has_solveable_islands() const {
        return !skipped && (stats.fullyActiveCount > 0u || stats.mixedSleepCount > 0u);
    bool has_solveable_islands() const { return reason == IslandSleepGraphRejectReason::None; }
    bool has_solveable_islands() const { return !skipped && reason == IslandSleepGraphRejectReason::None; }
};

/// Aggregate wake counts for graph-level batch guards.
struct IslandWakeGraphStats {
    u32 wakeableCount = 0;

/// Graph-level wake preflight for selective per-island sleeper activation.
struct IslandWakeGraphPreflight {
    IslandWakeGraphRejectReason reason = IslandWakeGraphRejectReason::None;
    IslandWakeGraphStats stats{};

    bool can_wake() const { return !skipped && stats.wakeableCount > 0u; }

/// Per-island wake outcome (skip vs activate) for guarded entry points.
struct IslandWakeResult {
    bool woke = false;
    bool skipped = false;
    u32 islandIndex = ContactIslandGraph::invalidIsland;
    u32 sleepersWoken = 0;
};

/// Batch wake summary for graph-level guarded activation.
struct IslandWakeBatchResult {
    u32 wokeCount = 0;
    u32 skippedCount = 0;
    u32 wakeableCount = 0;

    bool any_woke() const { return wokeCount > 0u; }

/// Combined solve pipeline preflight (dispatch + constraint refs + body participation).
struct IslandSolvePipelinePreflight {
    IslandDispatchPreflight dispatch{};
    IslandConstraintSolvePreflight constraintSolve{};
    IslandSleepPreflight sleep{};

    bool can_solve() const {
        return !skipped && dispatch.can_dispatch() && constraintSolve.can_solve() && !sleep.can_skip_solve();
    }

/// Combined wake + sleep + constraint-solve preflight for one island solve pipeline.
    IslandWakePreflight wake{};
    IslandConstraintSolvePreflight constraint{};

    bool should_wake_first() const { return wake.should_wake_sleepers(); }

        return !skipped && !sleep.can_skip_solve() && constraint.can_solve();

/// Combined graph dispatch + sleep/wake preflight for body-aware batch guards.
struct IslandDispatchBodiesPreflight {
    IslandSleepGraphPreflight sleep{};
    IslandWakeGraphPreflight wake{};

    bool can_dispatch() const {
        return !skipped && dispatch.can_dispatch() && sleep.has_solveable_islands();

/// Batch dispatch summary with sleep/wake pipeline outcomes.
struct IslandPipelineBatchDispatchResult {
    u32 solvedCount = 0;
    u32 sleepingSkippedCount = 0;
    u32 wakeCount = 0;
    u32 dispatchableCount = 0;

    bool any_solved() const { return solvedCount > 0u; }

/// Per-island wake outcome (skip vs activate) for parallel batch stubs.

/// Batch wake summary for parallel iteration stubs.
struct IslandBatchWakeResult {


/// Combined dispatch preflight (graph + timestep + sleep-state guards).
struct IslandSleepAwareDispatchPreflight {

    bool can_dispatch() const { return !skipped && dispatch.can_dispatch() && sleep.has_solveable_islands(); }

/// Batch sleep-aware dispatch summary for parallel iteration stubs.
struct IslandSleepAwareBatchDispatchResult {

    bool can_wake() const { return reason == IslandWakeGraphRejectReason::None; }
    bool can_wake() const { return !skipped && reason == IslandWakeGraphRejectReason::None; }
};

/// Combined dispatch + sleep graph preflight for batch solve guards (B4.4 deepen).
struct IslandSleepDispatchPreflight {
    IslandDispatchPreflight dispatch{};
    IslandSleepGraphPreflight sleep{};
    bool skipped = false;

    bool can_dispatch() const {
        return !skipped && dispatch.can_dispatch() && sleep.has_solveable_islands();
    }
};

/// Batch wake-then-solve summary for parallel dispatch stubs (B4.4 deepen).
struct IslandBatchSleepDispatchResult {
    u32 solvedCount = 0;
    u32 skippedCount = 0;
    u32 solveableCount = 0;
    u32 wokeCount = 0;
    u32 bodiesWoken = 0;
    bool skipped = false;

    bool any_solved() const { return solvedCount > 0u; }
};

/// Graph-level dispatch preflight combining wake, sleep, and constraint-solve guards.
struct IslandDispatchSolveablePreflight {
    IslandDispatchPreflight dispatch{};
    IslandWakeGraphPreflight wake{};
    IslandConstraintSolveGraphPreflight constraintSolve{};
    bool skipped = false;

    bool can_dispatch() const {
        return !skipped && dispatch.can_dispatch() && constraintSolve.can_dispatch();
    }
};

/// Batch wake-then-solve summary for parallel iteration stubs.
struct IslandBatchDispatchSolveableResult {
    u32 wokeCount = 0;
    u32 solvedCount = 0;
    u32 skippedCount = 0;
    u32 solveableCount = 0;
    bool skipped = false;

    bool any_solved() const { return solvedCount > 0u; }
};

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
    u32 noPriorDataCount = 0;

/// Graph-level warm-start preflight for selective per-island seeding (B4.4 deepen pass).
struct IslandWarmStartGraphPreflight {
    IslandWarmStartStats stats{};

    bool can_warm_start() const { return !skipped && stats.warmStartableCount > 0u; }

/// Graph-level warm-start preflight for selective per-island seeding (B4.4 deepen).
struct GraphWarmStartPreflight {
    u32 skippedEmptyCount = 0;
    u32 priorDistanceCoverage = 0;
    u32 priorContactCoverage = 0;

    bool can_warm_start() const {
        return !skipped && dispatchableCount > 0u &&
               (priorDistanceCoverage > 0u || priorContactCoverage > 0u);
    }

/// Batch warm-start summary for parallel island seeding stubs.
struct IslandWarmStartBatchResult {
    u32 skippedNoPriorCount = 0;


/// Batch warm-start summary for parallel selective seeding stubs.


/// Lightweight view for parallel warm-start dispatch (B4.4 deepen).
struct IslandWarmStartJob {
    u32 ownedDistanceCount = 0;
    u32 ownedContactCount = 0;
    bool empty = true;
    bool canWarmStart = false;
    const ContactIslandGraph::Island* island = nullptr;



/// Per-island contact impulse warm-start preflight (dt + empty-island guard).

    bool can_warm_start() const { return !skipped && !invalidDt && ownedContactCount > 0u; }

/// Constraint index validation for island solve preflight (B4.4 deepen).
struct IslandConstraintIndexValidation {
    u32 invalidContactIndices = 0;
    u32 invalidDistanceIndices = 0;
    bool valid = true;

    bool has_invalid_indices() const { return !valid; }

/// Per-island solve preflight including constraint index checks (B4.4 deepen).
    IslandSolveJob job{};
    IslandConstraintIndexValidation indices{};

    bool can_dispatch() const {
        return !skipped && !job.empty && job.island != nullptr && job.constraintCount > 0u && indices.valid;

/// Per-island contact-impulse warm-start preflight (B4.4 deepen).
    u32 priorImpulseCoverage = 0;

        return !skipped && !invalidDt && priorImpulseCoverage > 0u;

/// Aggregate contact-impulse warm-start counts for graph-level batch guards (B4.4 deepen).
    u32 noImpulseDataCount = 0;

/// Graph-level contact-impulse warm-start preflight (B4.4 deepen).

/// Graph-level combined warm-start preflight for batch guards.
struct IslandCombinedWarmStartGraphPreflight {
    IslandCombinedWarmStartStats stats{};

        return !skipped && !invalidDt && stats.warmStartableCount > 0u;

/// Per-island contact-impulse warm-start outcome (B4.4 deepen).
struct IslandContactImpulseWarmStartResult {

/// Combined lambda + contact-impulse warm-start preflight (B4.4 deepen).
    IslandWarmStartPreflight lambda{};
    IslandContactImpulseWarmStartPreflight impulse{};

        return !skipped && (lambda.can_warm_start() || impulse.can_warm_start());

/// Per-island solve input bounds preflight (B4.4 deepen pass).
struct IslandSolveInputsPreflight {
    bool contactsInRange = true;
    bool distancesInRange = true;

    bool can_solve() const { return !skipped && contactsInRange && distancesInRange; }

/// Contact-impulse warm-start preflight for per-island seeding (B4.4 deepen pass).
struct IslandContactImpulsePreflight {

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

struct IslandContactImpulseStats {
    u32 seedableCount = 0;

struct IslandContactImpulseGraphPreflight {
    IslandContactImpulseStats stats{};

    bool can_warm_start() const { return !skipped && !invalidDt && stats.seedableCount > 0u; }

/// Batch contact-impulse warm-start summary for parallel iteration stubs.
struct IslandContactImpulseBatchResult {



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
    u32 staleContactCount = 0;
    u32 validDistanceCount = 0;
    u32 staleDistanceCount = 0;
    bool hasStaleIndices = false;

        return !skipped && !hasStaleIndices && (validContactCount > 0u || validDistanceCount > 0u);

/// Dispatch preflight for one island index (job extraction + timestep guard).
struct IslandDispatchIndexPreflight {


/// True when the body carries `RB_SLEEPING`.
bool is_body_sleeping(const RigidBodySoA& bodies, u32 bodyIndex);

/// True when the body carries `RB_STATIC` or `RB_KINEMATIC`.
bool is_body_static_or_kinematic(const RigidBodySoA& bodies, u32 bodyIndex);

/// True when the body is dynamic (not static or kinematic).
bool is_body_dynamic(const RigidBodySoA& bodies, u32 bodyIndex);

/// Preflight constraint solve for one island; sets `skipped` for empty islands.
IslandSolveJobPreflight preflight_solve_island_job(

/// Preflight constraint solve by island index; out-of-range indices are marked skipped.
IslandSolveJobPreflight preflight_solve_island_job_by_index(
    u32 islandIndex,

/// Early-out guard when every dynamic body in the island is sleeping.
bool should_skip_solve_island_all_sleeping(const ContactIslandGraph::Island& island,

/// Early-out guard when the island has no dynamic bodies.
bool should_skip_solve_island_all_static(const ContactIslandGraph::Island& island,

/// Early-out guard when preflight reports no solvable constraint pairs.
bool should_skip_solve_island_job_preflight(const IslandSolveJobPreflight& preflight);

/// Preflight per-island sleep state; sets `skipped` for empty body lists.
IslandSleepPreflight preflight_island_sleep_state(const ContactIslandGraph::Island& island,

/// Preflight per-island sleep state by island index; out-of-range indices are marked skipped.
IslandSleepPreflight preflight_island_sleep_state_by_index(const ContactIslandGraph& graph,

/// Early-out guard for sleep detection on bodies with pending forces or `RB_CCD`.
bool should_skip_sleep_detection_for_body(const RigidBodySoA& bodies, u32 bodyIndex);

/// True when a sleeping body in the island should wake due to active contacts.
bool should_wake_island_on_contact(const ContactIslandGraph::Island& island,
                                   const std::vector<narrowphase::ContactManifold>& contacts);

/// Combined sleep/wake preflight for one island.
IslandSleepWakePreflight preflight_island_sleep_wake(

/// Combined sleep/wake preflight by island index; out-of-range indices are marked skipped.
IslandSleepWakePreflight preflight_island_sleep_wake_by_index(
/// Per-island sleep preflight for constraint-solve early-out (B4.4 deepen follow-up).
    u32 dynamicBodyCount = 0;
    u32 sleepingBodyCount = 0;
    u32 awakeBodyCount = 0;

    bool can_skip_solve() const { return !skipped && allSleeping && dynamicBodyCount > 0u; }

/// Per-island wake preflight for selective wake dispatch (B4.4 deepen follow-up).
    u32 wakeCandidateCount = 0;
    bool hasAwakeNeighborContact = false;

    bool should_wake() const { return !skipped && wakeCandidateCount > 0u; }

/// Combined constraint-solve preflight (empty, sleep, index guards) (B4.4 deepen follow-up).
    u32 outOfRangeContactCount = 0;
    u32 outOfRangeDistanceCount = 0;

        return !skipped && !empty && !invalidDt && !sleep.can_skip_solve();

/// Graph-level sleep stats for batch solve guards.
struct IslandSleepStats {
    u32 awakeCount = 0;

/// Graph-level sleep preflight for awake-only dispatch stubs.
    IslandSleepStats stats{};

    bool can_dispatch_awake() const { return !skipped && stats.awakeCount > 0u; }
/// Per-island body sleep-state counts for solve dispatch (B4.4 deepen follow-up).
    u32 totalBodies = 0;
    u32 staticCount = 0;
    u32 activeCount = 0;

    bool fullySleeping = false;

    bool can_solve() const { return !skipped && !fullySleeping; }

    bool hasExternalForce = false;
    bool hasVelocityWake = false;

    bool can_wake() const { return !skipped && (hasExternalForce || hasVelocityWake); }

/// Graph-level sleep preflight for batch solve dispatch (B4.4 deepen follow-up).
    u32 fullySleepingIslandCount = 0;
    u32 activeIslandCount = 0;

    bool has_active_islands() const { return activeIslandCount > 0u; }

/// Combined constraint-solve preflight (empty island, dt, sleep guards).
    bool emptyIsland = false;

        return !skipped && !invalidDt && !emptyIsland && sleep.can_solve() && constraintCount > 0u;
    bool allDynamicSleeping = false;

    bool can_solve() const { return !skipped && !allDynamicSleeping; }

/// Per-island wake preflight for sleeping bodies with external impetus (B4.4 deepen follow-up).

    bool needs_wake() const { return !skipped && wakeCandidateCount > 0u; }

/// Aggregate sleep counts for graph-level constraint-solve guards.
    u32 hasAwakeCount = 0;

/// Graph-level sleep preflight for batch constraint-solve guards.

    bool has_awake_islands() const { return !skipped && stats.hasAwakeCount > 0u; }

/// Combined constraint-solve preflight (dispatch + sleep + timestep guard).

        return !skipped && !invalidDt && solve.can_dispatch() && sleep.can_solve();

/// Per-island constraint solve preflight (dt + empty + all-sleeping guards, B4.4 deepen follow-up).
    u32 awakeDynamicCount = 0;
    u32 sleepingDynamicCount = 0;

        return !skipped && !invalidDt && constraintCount > 0u && !allDynamicSleeping;

/// Per-island sleep eligibility preflight (velocity threshold stub, B4.4 deepen follow-up).
    u32 belowThresholdCount = 0;

    bool can_consider_sleep() const {
        return !skipped && dynamicBodyCount > 0u && belowThresholdCount > 0u;

    bool all_dynamic_sleeping() const {
        return !skipped && dynamicBodyCount > 0u && sleepingBodyCount == dynamicBodyCount;

/// Per-island wake eligibility preflight (contacts/constraints/forces stub, B4.4 deepen follow-up).
    bool hasExternalForces = false;

    bool should_wake() const {
        return !skipped && sleepingBodyCount > 0u &&
               (ownedContactCount > 0u || ownedDistanceCount > 0u || hasExternalForces ||
                awakeBodyCount > 0u);

/// Graph-level sleep/wake summary for batch guards (B4.4 deepen follow-up).
struct IslandSleepWakeStats {
    u32 sleepCandidateCount = 0;

/// True when body index is in range for island build input validation.
bool is_valid_island_build_body_index(u32 bodyIndex, u32 bodyCount);

/// True when body count is valid for island graph construction.
bool is_valid_island_build_body_count(u32 bodyCount);

/// True when a body carries `RB_SLEEPING`.
bool is_rigid_body_sleeping(u32 bodyFlags);

/// True when a body carries `RB_STATIC` or `RB_KINEMATIC`.
bool is_rigid_body_static_or_kinematic(u32 bodyFlags);

/// True when a body is dynamic (not static, kinematic, or sleeping).
bool is_rigid_body_dynamic_awake(u32 bodyFlags);

/// True when every dynamic body in the island is sleeping.
bool is_island_all_sleeping(const ContactIslandGraph::Island& island, const RigidBodySoA& bodies);

/// True when the island has at least one sleeping body adjacent to an awake body via contacts.
bool island_has_wake_candidates(const ContactIslandGraph::Island& island,
                                const RigidBodySoA& bodies,
                                const std::vector<narrowphase::ContactManifold>& contacts);

/// Per-island body index coverage for constraint solve (stale/out-of-range guards).
struct IslandSolveBodyRefsPreflight {
    u32 ownedBodyCount = 0;
    u32 inRangeBodyCount = 0;
    u32 sleepingBodyCount = 0;
    u32 staticBodyCount = 0;
    bool skipped = false;

    bool can_solve() const { return !skipped && inRangeBodyCount > 0u; }
};

/// Per-island sleep/wake diagnostics for solve dispatch (B4.4 deepen pass).
struct IslandSleepWakePreflight {
    u32 totalBodies = 0;
    u32 sleepingBodyCount = 0;
    u32 awakeDynamicCount = 0;
    u32 staticBodyCount = 0;
    bool skipped = false;

    bool all_dynamic_sleeping() const {
        return !skipped && awakeDynamicCount == 0u && sleepingBodyCount > 0u;
    }
    bool can_solve() const { return !skipped && awakeDynamicCount > 0u; }
    bool can_sleep() const {
        return !skipped && sleepingBodyCount + staticBodyCount == totalBodies && totalBodies > 0u;
    }
};

/// Wake preflight when constraints exist on a sleeping island (B4.4 deepen pass).
struct IslandWakePreflight {
    u32 sleepingDynamicCount = 0;
    u32 constrainedBodyCount = 0;
    bool hasConstraints = false;
    bool skipped = false;

    bool needs_wake() const { return !skipped && hasConstraints && sleepingDynamicCount > 0u; }
    bool can_wake() const { return needs_wake(); }
};

/// Combined constraint-solve preflight (job + refs + bodies + sleep/wake guards).
struct IslandConstraintSolvePreflight {
    IslandSolveJobPreflight job{};
    IslandConstraintRefsPreflight refs{};
    IslandSolveBodyRefsPreflight bodies{};
    IslandSleepWakePreflight sleepWake{};
    bool skipped = false;

    bool can_solve() const {
        return !skipped && job.can_dispatch() && refs.can_solve() && bodies.can_solve() &&
               sleepWake.can_solve();
    }
};

/// Aggregate sleep/wake counts for graph-level batch guards.
struct IslandSleepWakeStats {
    u32 totalIslands = 0;
    u32 solvableCount = 0;
    u32 allSleepingCount = 0;
    u32 emptyCount = 0;
};

/// Graph-level sleep/wake preflight for selective per-island solve dispatch.
struct IslandSleepWakeGraphPreflight {
    IslandSleepWakeStats stats{};
    bool skipped = false;

    bool can_solve_any() const { return !skipped && stats.solvableCount > 0u; }
};

/// Wake-then-solve preflight for one island (B4.4 deepen).
struct IslandWakeAndSolvePreflight {
    IslandWakePreflight wake{};
    IslandConstraintSolvePreflight solve{};
    IslandSleepPreflight sleep{};
    bool skipped = false;

    bool can_solve() const { return !skipped && solve.can_solve() && !sleep.can_skip_solve(); }
    bool should_wake_first() const { return !skipped && wake.should_wake_sleepers(); }
};

/// Per-island wake-then-solve outcome (B4.4 deepen).
struct IslandWakeAndSolveResult {
    bool woke = false;
    bool solved = false;
    bool skipped = false;
    u32 islandIndex = ContactIslandGraph::invalidIsland;
};

/// Batch wake-and-solve summary (B4.4 deepen).
struct IslandBatchWakeAndSolveResult {
    u32 wokeCount = 0;
    u32 solvedCount = 0;
    u32 skippedCount = 0;
    u32 solveableCount = 0;
    bool skipped = false;

    bool any_solved() const { return solvedCount > 0u; }
};

/// Graph-level wake-and-solve preflight (B4.4 deepen).
struct IslandWakeAndSolveGraphPreflight {
    IslandWakeGraphPreflight wake{};
    IslandSleepGraphPreflight sleep{};
    IslandSolvePreflight solve{};
    bool skipped = false;

    bool can_dispatch() const { return !skipped && solve.can_dispatch() && sleep.has_solveable_islands(); }
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
/// True when the body carries `RB_SLEEPING`.
bool is_body_sleeping(const RigidBodySoA& bodies, u32 bodyIndex);

/// True when the body is static or kinematic.
bool is_body_static_or_kinematic(const RigidBodySoA& bodies, u32 bodyIndex);

/// True when the body can receive constraint corrections (non-static, awake).
bool body_has_effective_mass(const RigidBodySoA& bodies, u32 bodyIndex);

/// True when every dynamic body in the island is sleeping.
bool island_all_bodies_sleeping(const ContactIslandGraph::Island& island, const RigidBodySoA& bodies);

/// True when at least one body in the island can move under constraints.
bool island_has_movable_bodies(const ContactIslandGraph::Island& island, const RigidBodySoA& bodies);

/// True when at least one non-static, non-sleeping body is in the island.
bool island_has_awake_dynamic_bodies(const ContactIslandGraph::Island& island, const RigidBodySoA& bodies);

/// Preflight orphaned contact/distance indices for one island.

/// Preflight sleep/mobility for one island; sets `skipped` for empty islands.
IslandSleepPreflight preflight_sleeping_island(const ContactIslandGraph::Island& island,
                                               const RigidBodySoA& bodies);

/// Preflight sleep/mobility by island index; out-of-range indices are marked skipped.
IslandSleepPreflight preflight_sleeping_island_by_index(const ContactIslandGraph& graph,

/// Early-out guard when every dynamic body in the island is sleeping.
bool should_skip_sleeping_island_solve(const ContactIslandGraph::Island& island,

/// Early-out guard combining job validity and sleeping-island preflight.
bool should_skip_sleeping_island_solve_job(const IslandSolveJob& job,

/// Preflight wake-on-impulse for one body; out-of-range indices are marked skipped.
WakeOnImpulsePreflight preflight_wake_on_impulse(const RigidBodySoA& bodies,
                                                 u32 bodyIndex,
                                                 f32 wakeEpsilonSq = 1e-8f);

/// True when a sleeping body should wake from applied force/torque above epsilon.
bool should_wake_body_on_impulse(const RigidBodySoA& bodies,

/// Preflight body-partition validity across all islands (diagnostic).
IslandBodyPartitionPreflight preflight_island_body_partition(const ContactIslandGraph& graph);

/// Preflight constraint-iteration params; sets `skipped` when iterations is zero.
ConstraintIterationPreflight preflight_constraint_iterations(const SolverParams& params);

/// Early-out guard when constraint iterations are zero.
bool should_skip_constraint_iterations(const SolverParams& params);

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

/// Preflight constraint refs with live body-index coverage; `bodyCount == 0` skips body checks.
IslandConstraintRefsPreflight preflight_island_constraint_refs(
    const ContactIslandGraph::Island& island,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints,
    u32 bodyCount);

/// Early-out guard when an island has no in-range constraint references to solve.
bool should_skip_island_constraint_refs(const ContactIslandGraph::Island& island,

/// True when dt is positive for contact impulse warm-start seeding.
/// True when `flags` carries `RB_SLEEPING`.
bool is_body_sleeping(u32 flags);

/// True when `flags` carries `RB_STATIC` or `RB_KINEMATIC`.
bool is_body_static_or_kinematic(u32 flags);

/// Summarize sleeping vs active bodies in one island.
IslandSleepStats compute_island_sleep_stats(const RigidBodySoA& bodies,
                                            const ContactIslandGraph::Island& island);

/// True when every dynamic body in the island is sleeping.
bool is_island_fully_sleeping(const RigidBodySoA& bodies, const ContactIslandGraph::Island& island);

/// Preflight island sleep state; sets `skipped` for empty islands.
IslandSleepPreflight preflight_island_sleep(const RigidBodySoA& bodies,

/// Preflight island sleep by index; out-of-range indices are marked skipped.
IslandSleepPreflight preflight_island_sleep_by_index(const RigidBodySoA& bodies,
                                                    const ContactIslandGraph& graph,
                                                    u32 islandIndex);

/// Early-out guard for solving a fully sleeping island.
bool should_skip_solve_sleeping_island(const RigidBodySoA& bodies,

/// Early-out guard for solving a fully sleeping island by index.
bool should_skip_solve_sleeping_island_index(const RigidBodySoA& bodies,

/// Preflight wake candidates (external force or velocity above threshold).
IslandWakePreflight preflight_island_wake(const RigidBodySoA& bodies,
                                          f32 linearWakeThreshold,
                                          f32 angularWakeThreshold);

/// Preflight wake by island index; out-of-range indices are marked skipped.
IslandWakePreflight preflight_island_wake_by_index(const RigidBodySoA& bodies,
                                                   u32 islandIndex,

/// True when the island should be woken this frame.
bool should_wake_island(const RigidBodySoA& bodies,

/// Preflight constraint solve for one island (dt + empty + sleep guards).
IslandConstraintSolvePreflight preflight_island_constraint_solve(const RigidBodySoA& bodies,
                                                                 f32 dt);

/// Preflight constraint solve by island index; out-of-range indices are marked skipped.
IslandConstraintSolvePreflight preflight_island_constraint_solve_by_index(const RigidBodySoA& bodies,

/// Early-out guard combining dt, empty-island, and sleep preflights.
bool should_skip_island_constraint_solve(const RigidBodySoA& bodies,

/// Graph-level sleep preflight for batch solve dispatch.
IslandSleepGraphPreflight preflight_island_sleep_graph(const RigidBodySoA& bodies,
                                                       const ContactIslandGraph& graph);

/// True when every constrained island is fully sleeping.
bool should_skip_island_sleep_dispatch(const RigidBodySoA& bodies, const ContactIslandGraph& graph);

/// Collect island indices that are not fully sleeping and carry constraints.
std::vector<u32> collect_awake_island_indices(const RigidBodySoA& bodies,

/// Guarded dispatch that skips fully sleeping islands; valid paths unchanged for awake islands.
bool dispatch_solve_awake_island(RigidBodySoA& bodies,
                                 SolverWorkBuffers& workBuffers,
                                 const std::vector<DistanceConstraint>& distanceConstraints,
                                 f32 dt,
                                 f32 contactCompliance,
                                 const std::function<f32(const RigidBodySoA&, u32)>& invMassFn);

/// Guarded awake dispatch with explicit skip/solve outcome.
IslandDispatchResult dispatch_solve_awake_island_result(RigidBodySoA& bodies,
/// True when body flags include `RB_SLEEPING`.

/// True when body is static or kinematic (non-dynamic stub).

/// True when body is dynamic and not sleeping.
bool is_body_dynamic_awake(const RigidBodySoA& bodies, u32 bodyIndex);

/// True when every dynamic body in the island carries `RB_SLEEPING`.
bool island_all_dynamic_bodies_sleeping(const RigidBodySoA& bodies, const ContactIslandGraph::Island& island);

/// True when at least one dynamic body in the island is awake.
bool island_has_awake_dynamic_bodies(const RigidBodySoA& bodies, const ContactIslandGraph::Island& island);

/// Count awake dynamic bodies referenced by one island.
u32 count_island_awake_dynamic_bodies(const RigidBodySoA& bodies, const ContactIslandGraph::Island& island);

/// Preflight constraint solve for one island; sets `skipped` for empty or out-of-range islands.


/// Early-out guard for per-island constraint solve (empty, invalid dt, or all dynamic bodies sleeping).

/// Early-out guard for constraint solve by island index (empty, out-of-range, invalid dt, or all sleeping).
bool should_skip_island_constraint_solve_index(const RigidBodySoA& bodies,

/// Guarded constraint solve; returns false when preflight rejects the island.
bool solve_island_job_guarded(RigidBodySoA& bodies,

/// Preflight sleep eligibility for one island; sets `skipped` for empty or static-only islands.
                                            f32 sleepLinearThreshold,
                                            f32 sleepAngularThreshold);

/// Preflight sleep by island index; out-of-range indices are marked skipped.

/// Early-out guard for per-island sleep checks (empty or no dynamic bodies).
bool should_skip_island_sleep_check(const ContactIslandGraph::Island& island);

/// Early-out guard for sleep check by island index (empty, out-of-range, or no dynamic bodies).
bool should_skip_island_sleep_index(const ContactIslandGraph& graph, u32 islandIndex);

/// Preflight wake eligibility for one island; sets `skipped` for empty islands.
IslandWakePreflight preflight_island_wake(const RigidBodySoA& bodies, const ContactIslandGraph::Island& island);


/// Early-out guard for per-island wake checks (empty island).
bool should_skip_island_wake_check(const ContactIslandGraph::Island& island);

/// Early-out guard for wake check by island index (empty or out-of-range).
bool should_skip_island_wake_index(const ContactIslandGraph& graph, u32 islandIndex);

/// Summarize sleep/wake candidates across all islands for batch guards.
IslandSleepWakeStats compute_island_sleep_wake_stats(const RigidBodySoA& bodies,

/// True when `bodyIndex` carries `RB_SLEEPING`.
bool is_sleeping_body(const RigidBodySoA& bodies, u32 bodyIndex);

/// True when `bodyIndex` is dynamic (not static or kinematic).
bool is_dynamic_body(const RigidBodySoA& bodies, u32 bodyIndex);

/// True when `bodyIndex` is dynamic and not sleeping.
bool is_awake_dynamic_body(const RigidBodySoA& bodies, u32 bodyIndex);

/// True when every body in the island carries `RB_SLEEPING`.
bool island_all_bodies_sleeping(const ContactIslandGraph::Island& island, const RigidBodySoA& bodies);

/// True when the island has at least one awake dynamic body.
bool island_has_awake_dynamic_body(const ContactIslandGraph::Island& island, const RigidBodySoA& bodies);

/// Preflight sleep state for one island; sets `skipped` for empty islands.
IslandSleepPreflight preflight_island_sleep(const ContactIslandGraph::Island& island,
                                            const RigidBodySoA& bodies);

/// Preflight sleep state by island index; out-of-range indices are marked skipped.
IslandSleepPreflight preflight_island_sleep_by_index(const ContactIslandGraph& graph,
                                                     u32 islandIndex,
                                                     const RigidBodySoA& bodies);

/// Summarize solvable vs all-sleeping islands for batch guards.
IslandSleepStats compute_island_sleep_stats(const ContactIslandGraph& graph, const RigidBodySoA& bodies);

/// Count islands that pass per-island sleep solve preflight.
u32 count_solvable_sleep_islands(const ContactIslandGraph& graph, const RigidBodySoA& bodies);

/// True when at least one island has an awake dynamic body to solve.
bool has_solvable_sleep_islands(const ContactIslandGraph& graph, const RigidBodySoA& bodies);

/// Early-out guard when every dynamic body in the island is sleeping.
bool should_skip_island_solve_sleeping(const ContactIslandGraph::Island& island,
                                       const RigidBodySoA& bodies);

/// Preflight wake candidates for sleeping bodies in one island.
IslandWakePreflight preflight_island_wake(const ContactIslandGraph::Island& island,
                                          const RigidBodySoA& bodies,
                                          const std::vector<narrowphase::ContactManifold>& contacts);

/// Early-out guard when no sleeping bodies in the island need waking.
bool should_skip_island_wake(const ContactIslandGraph::Island& island,
                             const RigidBodySoA& bodies,
                             const std::vector<narrowphase::ContactManifold>& contacts);

/// Guarded wake: clears `RB_SLEEPING` on bodies flagged by preflight; returns count woken.
u32 wake_island_bodies_guarded(RigidBodySoA& bodies,
                               const ContactIslandGraph::Island& island,
                               const std::vector<narrowphase::ContactManifold>& contacts);

/// Preflight one extracted island job including timestep and sleep validity.
IslandSolveJobSleepPreflight preflight_solve_island_job_with_sleep(const IslandSolveJob& job,
                                                                   f32 dt,
                                                                   const RigidBodySoA& bodies);

/// Early-out guard for job-level dispatch including sleep state.
bool should_skip_solve_island_job_with_sleep(const IslandSolveJob& job,
                                             f32 dt,
                                             const RigidBodySoA& bodies);

/// Preflight island dispatch including timestep and sleep coverage.
IslandSleepDispatchPreflight preflight_island_dispatch_with_sleep(const ContactIslandGraph& graph,
                                                                  f32 dt,
                                                                  const RigidBodySoA& bodies);

/// Early-out guard combining graph dispatch preflight and sleep coverage.
bool should_skip_island_dispatch_with_sleep(const ContactIslandGraph& graph,
                                            f32 dt,
                                            const RigidBodySoA& bodies);

/// Collect island indices that pass sleep solve preflight.
std::vector<u32> collect_solvable_sleep_island_indices(const ContactIslandGraph& graph,
                                                       const RigidBodySoA& bodies);

/// True when the body index is in range and carries `RB_SLEEPING`.
bool is_body_sleeping(const RigidBodySoA& bodies, u32 bodyIndex);

/// True when the body index is in range and is not sleeping.
bool is_body_awake(const RigidBodySoA& bodies, u32 bodyIndex);

/// True when the body index is in range and carries `RB_STATIC` or `RB_KINEMATIC`.
bool is_body_static_or_kinematic(const RigidBodySoA& bodies, u32 bodyIndex);

/// Count in-range bodies in `island` that carry `RB_SLEEPING`.
u32 count_sleeping_bodies_in_island(const ContactIslandGraph::Island& island,
                                    const RigidBodySoA& bodies);

/// True when every in-range dynamic body in the island is sleeping.
bool island_all_bodies_sleeping(const ContactIslandGraph::Island& island,
                                const RigidBodySoA& bodies);

/// Preflight sleep state for one island solve pass; sets `skipped` for empty islands.
IslandSleepPreflight preflight_island_sleep_for_solve(const ContactIslandGraph::Island& island,
                                                      const RigidBodySoA& bodies);

/// Preflight sleep state by island index; out-of-range indices are marked skipped.
IslandSleepPreflight preflight_island_sleep_for_solve_by_index(const ContactIslandGraph& graph,
                                                               u32 islandIndex,
                                                               const RigidBodySoA& bodies);

/// Summarize solveable vs all-sleeping islands for batch guards.
IslandSleepGraphStats compute_island_sleep_graph_stats(const ContactIslandGraph& graph,
                                                       const RigidBodySoA& bodies);

/// Graph-level sleep preflight; sets `skipped` when no islands can be solved.
IslandSleepGraphPreflight preflight_island_sleep_graph(const ContactIslandGraph& graph,
                                                       const RigidBodySoA& bodies);

/// Early-out guard when every constrained island is all-sleeping.
bool should_skip_island_solve_for_sleep(const ContactIslandGraph& graph,
                                        const RigidBodySoA& bodies);

/// Early-out guard when an island's dynamic bodies are all sleeping.
bool should_skip_solve_sleeping_island(const ContactIslandGraph::Island& island,
                                       const RigidBodySoA& bodies);

/// Preflight wake candidates for one island; sets `skipped` for empty islands.
IslandWakePreflight preflight_island_wake(const ContactIslandGraph::Island& island,
                                          const RigidBodySoA& bodies,
                                          f32 linearSleepThreshold,
                                          f32 angularSleepThreshold);

/// Preflight wake candidates by island index; out-of-range indices are marked skipped.
IslandWakePreflight preflight_island_wake_by_index(const ContactIslandGraph& graph,
                                                   u32 islandIndex,
                                                   const RigidBodySoA& bodies,
                                                   f32 linearSleepThreshold,
                                                   f32 angularSleepThreshold);

/// Early-out guard when no sleeping bodies remain in the island.
bool should_skip_island_wake_check(const ContactIslandGraph::Island& island,
                                   const RigidBodySoA& bodies);

/// Combined per-job solve preflight (job + constraint refs + sleep + dt guards).
IslandSolveCombinedPreflight preflight_island_solve_combined(
    const IslandSolveJob& job,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints,
    f32 dt);

/// Early-out guard combining job, constraint refs, sleep, and timestep validity.
bool should_skip_island_solve_combined(const IslandSolveJob& job,
                                       const RigidBodySoA& bodies,
                                       const std::vector<narrowphase::ContactManifold>& contacts,
                                       const std::vector<DistanceConstraint>& distanceConstraints,
                                       f32 dt);

/// True when `bodyIndex` is in range for `bodies`.
bool body_index_in_range(const RigidBodySoA& bodies, u32 bodyIndex);

/// True when a body carries `RB_SLEEPING`.
bool is_sleeping_body(u32 bodyFlags);

/// True when a body is static or kinematic.
bool is_static_or_kinematic_body(u32 bodyFlags);

/// True when a body is dynamic and not sleeping.
bool is_active_dynamic_body(u32 bodyFlags);

/// Preflight body index coverage for one island; sets `skipped` for empty islands.
IslandBodyRefsPreflight preflight_island_body_refs(const ContactIslandGraph::Island& island,
                                                   const RigidBodySoA& bodies);

/// Early-out guard when an island references out-of-range body indices.
bool should_skip_island_body_refs(const ContactIslandGraph::Island& island, const RigidBodySoA& bodies);

/// True when every dynamic body in the island is sleeping.
bool is_island_all_sleeping(const ContactIslandGraph::Island& island, const RigidBodySoA& bodies);

/// True when the island has at least one active dynamic body.
bool is_island_wake_candidate(const ContactIslandGraph::Island& island, const RigidBodySoA& bodies);

/// Preflight sleep state for one island; sets `skipped` for empty islands.
IslandSleepPreflight preflight_island_sleep_state(const ContactIslandGraph::Island& island,
                                                  const RigidBodySoA& bodies);

/// Early-out guard when every dynamic body in the island is sleeping.
bool should_skip_sleeping_island_solve(const ContactIslandGraph::Island& island, const RigidBodySoA& bodies);

/// Preflight wake candidates for one island; sets `skipped` for empty islands.
IslandWakePreflight preflight_island_wake(const ContactIslandGraph::Island& island, const RigidBodySoA& bodies);

/// Early-out guard when no sleeping body in the island has external force to wake.
bool should_skip_island_wake(const ContactIslandGraph::Island& island, const RigidBodySoA& bodies);

/// Combined constraint solve preflight for one island.
IslandConstraintSolvePreflight preflight_island_constraint_solve(
    const ContactIslandGraph::Island& island,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints,
    f32 dt);

/// Early-out guard combining constraint refs, body refs, sleep state, and dt validity.
bool should_skip_island_constraint_solve(const ContactIslandGraph::Island& island,
                                         const RigidBodySoA& bodies,
                                         const std::vector<narrowphase::ContactManifold>& contacts,
                                         const std::vector<DistanceConstraint>& distanceConstraints,
                                         f32 dt);

/// Guarded island solve with sleep/body-ref preflights; returns false when preflight rejects.
bool solve_island_job_guarded(RigidBodySoA& bodies,
                              const ContactIslandGraph::Island& island,
                              SolverWorkBuffers& workBuffers,
                              const std::vector<DistanceConstraint>& distanceConstraints,
                              f32 dt,
                              f32 contactCompliance,
                              const std::function<f32(const RigidBodySoA&, u32)>& invMassFn);

/// Preflight island graph build inputs without mutating a graph.
IslandBuildPreflight preflight_island_build(
    u32 bodyCount,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints);

/// Early-out guard when island graph build inputs are invalid.
bool should_skip_island_build(u32 bodyCount,
                              const std::vector<narrowphase::ContactManifold>& contacts,
                              const std::vector<DistanceConstraint>& distanceConstraints);

/// Preflight sleep state for one island; sets `skipped` for empty islands.
IslandSleepPreflight preflight_island_sleep(const ContactIslandGraph::Island& island,
                                            const RigidBodySoA& bodies);

/// Preflight sleep state by island index; out-of-range indices are marked skipped.
IslandSleepPreflight preflight_island_sleep_by_index(const ContactIslandGraph& graph,
                                                     u32 islandIndex,
                                                     const RigidBodySoA& bodies);

/// Summarize all-sleeping vs partially-awake islands for batch guards.
IslandSleepStats compute_island_sleep_stats(const ContactIslandGraph& graph,
                                            const RigidBodySoA& bodies);

/// Count islands where all dynamic bodies are sleeping.
u32 count_all_sleeping_islands(const ContactIslandGraph& graph, const RigidBodySoA& bodies);

/// True when at least one island has awake dynamic bodies.
bool has_awake_islands(const ContactIslandGraph& graph, const RigidBodySoA& bodies);

/// Graph-level sleep preflight; sets `skipped` when graph is empty.
IslandSleepGraphPreflight preflight_island_sleep_graph(const ContactIslandGraph& graph,
                                                       const RigidBodySoA& bodies);

/// Early-out guard for graph-level solve when every constrained island is all-sleeping.
bool should_skip_island_solve_all_sleeping(const ContactIslandGraph& graph,
                                           const RigidBodySoA& bodies);

/// Early-out guard when an island's dynamic bodies are all sleeping.
bool should_skip_sleeping_island(const ContactIslandGraph::Island& island,
                                 const RigidBodySoA& bodies);

/// Early-out guard for solve by island index (empty, out-of-range, or all-sleeping).
bool should_skip_sleeping_island_index(const ContactIslandGraph& graph,
                                       u32 islandIndex,
                                       const RigidBodySoA& bodies);

/// Collect island indices that are not all-sleeping (parallel dispatch prep).
std::vector<u32> collect_awake_island_indices(const ContactIslandGraph& graph,
                                              const RigidBodySoA& bodies);

/// Preflight wake candidates for one island; sets `skipped` for empty islands.
IslandWakePreflight preflight_island_wake(const ContactIslandGraph::Island& island,
                                          const RigidBodySoA& bodies,
                                          const std::vector<narrowphase::ContactManifold>& contacts);

/// Preflight wake candidates by island index; out-of-range indices are marked skipped.
IslandWakePreflight preflight_island_wake_by_index(const ContactIslandGraph& graph,
                                                   u32 islandIndex,
                                                   const RigidBodySoA& bodies,
                                                   const std::vector<narrowphase::ContactManifold>& contacts);

/// Summarize wake-candidate vs no-wake islands for batch guards.
IslandWakeStats compute_island_wake_stats(const ContactIslandGraph& graph,
                                          const RigidBodySoA& bodies,
                                          const std::vector<narrowphase::ContactManifold>& contacts);

/// Count islands with at least one wake candidate.
u32 count_wake_candidate_islands(const ContactIslandGraph& graph,
                                 const RigidBodySoA& bodies,
                                 const std::vector<narrowphase::ContactManifold>& contacts);

/// True when at least one island has sleeping bodies adjacent to awake neighbors.
bool has_wake_candidate_islands(const ContactIslandGraph& graph,
                                const RigidBodySoA& bodies,
                                const std::vector<narrowphase::ContactManifold>& contacts);

/// Graph-level wake preflight; sets `skipped` when graph is empty.
IslandWakeGraphPreflight preflight_island_wake_graph(const ContactIslandGraph& graph,
                                                     const RigidBodySoA& bodies,
                                                     const std::vector<narrowphase::ContactManifold>& contacts);

/// Early-out guard for wake dispatch when no candidates exist.
bool should_skip_island_wake_graph(const ContactIslandGraph& graph,
                                   const RigidBodySoA& bodies,
                                   const std::vector<narrowphase::ContactManifold>& contacts);

/// Collect body indices in an island that should wake due to awake neighbors.
std::vector<u32> collect_island_wake_body_indices(const ContactIslandGraph::Island& island,
                                                  const RigidBodySoA& bodies,
                                                  const std::vector<narrowphase::ContactManifold>& contacts);

/// True when `bodyIndex` references a sleeping body.
bool island_body_is_sleeping(const RigidBodySoA& bodies, u32 bodyIndex);

/// True when `bodyIndex` references a static or kinematic body.
bool island_body_is_static_or_kinematic(const RigidBodySoA& bodies, u32 bodyIndex);

/// True when `bodyIndex` references an awake dynamic body.
bool island_body_is_awake_dynamic(const RigidBodySoA& bodies, u32 bodyIndex);

/// Preflight sleep/wake state for one island; sets `skipped` for empty islands.
IslandSleepWakePreflight preflight_island_sleep_wake(const ContactIslandGraph::Island& island,
                                                     const RigidBodySoA& bodies);

/// Early-out guard when every in-range island body is sleeping.
bool should_skip_solve_sleeping_island(const ContactIslandGraph::Island& island,
                                       const RigidBodySoA& bodies);

/// Combined constraint-ref + sleep/wake preflight for one island solve pass.
IslandConstraintSolvePreflight preflight_island_constraint_solve(
    const ContactIslandGraph::Island& island,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints);

/// Early-out guard combining constraint refs and sleep/wake state.
bool should_skip_island_constraint_solve(const ContactIslandGraph::Island& island,
                                         const RigidBodySoA& bodies,
                                         const std::vector<narrowphase::ContactManifold>& contacts,
                                         const std::vector<DistanceConstraint>& distanceConstraints);

/// Summarize solvable vs all-sleeping islands for graph-level guards.
IslandSleepWakeStats compute_island_sleep_wake_stats(const ContactIslandGraph& graph,
                                                     const RigidBodySoA& bodies);

/// Count islands that pass per-island sleep/wake solve preflight.
u32 count_solvable_islands(const ContactIslandGraph& graph, const RigidBodySoA& bodies);

/// True when at least one island has awake dynamic bodies to solve.
bool has_solvable_islands(const ContactIslandGraph& graph, const RigidBodySoA& bodies);

/// Graph-level sleep/wake preflight; sets `skipped` when nothing can solve.
IslandSleepWakeGraphPreflight preflight_island_sleep_wake_graph(const ContactIslandGraph& graph,
                                                               const RigidBodySoA& bodies);

/// Early-out guard for graph-level solve when every constrained island is sleeping.
bool should_skip_island_solve_sleep_wake(const ContactIslandGraph& graph, const RigidBodySoA& bodies);

/// True when a body index is in range for island solve passes.
bool is_island_body_index_valid(u32 bodyIndex, u32 bodyCount);

/// True when a body carries `RB_SLEEPING`.
bool is_island_body_sleeping(const RigidBodySoA& bodies, u32 bodyIndex);

/// True when a body is static or kinematic.
bool is_island_body_static_or_kinematic(const RigidBodySoA& bodies, u32 bodyIndex);

/// Preflight body index coverage for one island; sets `skipped` for empty islands.
IslandSolveBodyRefsPreflight preflight_island_solve_bodies(const ContactIslandGraph::Island& island,
                                                           const RigidBodySoA& bodies);

/// Early-out guard when an island has no in-range body references to solve.
bool should_skip_island_solve_bodies(const ContactIslandGraph::Island& island,
                                     const RigidBodySoA& bodies);

/// Preflight sleep/wake state for one island; sets `skipped` for empty islands.
IslandSleepWakePreflight preflight_island_sleep_wake(const ContactIslandGraph::Island& island,
                                                     const RigidBodySoA& bodies);

/// Preflight sleep/wake by island index; out-of-range indices are marked skipped.
IslandSleepWakePreflight preflight_island_sleep_wake_by_index(const ContactIslandGraph& graph,
                                                              u32 islandIndex,
                                                              const RigidBodySoA& bodies);

/// Early-out guard when all dynamic bodies in an island are sleeping.
bool should_skip_island_solve_for_sleep(const ContactIslandGraph::Island& island,
                                        const RigidBodySoA& bodies);

/// Early-out guard for sleep skip by island index (empty, out-of-range, or all sleeping).
bool should_skip_island_solve_for_sleep_index(const ContactIslandGraph& graph,
                                              u32 islandIndex,
                                              const RigidBodySoA& bodies);

/// Preflight wake when constraints remain on a sleeping island.
IslandWakePreflight preflight_island_wake(const ContactIslandGraph::Island& island,
                                          const RigidBodySoA& bodies);

/// True when an island has constraints that should wake sleeping dynamic bodies.
bool should_wake_island(const ContactIslandGraph::Island& island, const RigidBodySoA& bodies);

/// Summarize solvable vs all-sleeping/empty islands for graph-level guards.
IslandSleepWakeStats compute_island_sleep_wake_stats(const ContactIslandGraph& graph,
                                                     const RigidBodySoA& bodies);

/// Graph-level sleep/wake preflight; sets `skipped` when nothing can be solved.
IslandSleepWakeGraphPreflight preflight_island_sleep_wake_graph(const ContactIslandGraph& graph,
                                                                const RigidBodySoA& bodies);

/// Early-out guard for graph-level solve batching when all islands are sleeping or empty.
bool should_skip_island_sleep_wake_graph(const ContactIslandGraph& graph, const RigidBodySoA& bodies);

/// Collect island indices that pass per-island sleep/wake solve preflight.
std::vector<u32> collect_solvable_island_indices(const ContactIslandGraph& graph,
                                                   const RigidBodySoA& bodies);

/// Combined constraint-solve preflight for one extracted island job.
IslandConstraintSolvePreflight preflight_island_constraint_solve(
    const IslandSolveJob& job,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints,
    f32 dt);

/// Early-out guard combining job, constraint-ref, body-ref, and sleep/wake preflights.
bool should_skip_island_constraint_solve(const IslandSolveJob& job,
                                         const RigidBodySoA& bodies,
                                         const std::vector<narrowphase::ContactManifold>& contacts,
                                         const std::vector<DistanceConstraint>& distanceConstraints,
                                         f32 dt);

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

/// True when `bodyIndex` carries `RB_SLEEPING`.
bool is_body_sleeping(const RigidBodySoA& bodies, u32 bodyIndex);

/// True when a body is static, kinematic, or sleeping (no constraint solve contribution).
bool is_body_solve_immobile(const RigidBodySoA& bodies, u32 bodyIndex);

/// Preflight awake vs immobile body coverage for one island; sets `skipped` for empty islands.
IslandSolveBodiesPreflight preflight_island_solve_bodies(const ContactIslandGraph::Island& island,
                                                         const RigidBodySoA& bodies);

/// Preflight awake vs immobile body coverage by island index; out-of-range indices are skipped.
IslandSolveBodiesPreflight preflight_island_solve_bodies_by_index(const ContactIslandGraph& graph,
                                                                  u32 islandIndex,
                                                                  const RigidBodySoA& bodies);

/// Early-out guard when every dynamic body in an island is immobile (sleeping/static/kinematic).
bool should_skip_island_solve_bodies(const ContactIslandGraph::Island& island,
                                    const RigidBodySoA& bodies);

/// Preflight sleep state for one island; sets `skipped` for empty islands.
IslandSleepPreflight preflight_island_sleep(const ContactIslandGraph::Island& island,
                                            const RigidBodySoA& bodies);

/// Preflight sleep state by island index; out-of-range indices are marked skipped.
IslandSleepPreflight preflight_island_sleep_by_index(const ContactIslandGraph& graph,
                                                     u32 islandIndex,
                                                     const RigidBodySoA& bodies);

/// Early-out guard when all dynamic bodies in an island are sleeping.
bool should_skip_sleeping_island_solve(const ContactIslandGraph::Island& island,
                                       const RigidBodySoA& bodies);

/// Early-out guard for sleeping island solve by index (empty, out-of-range, or all-sleeping).
bool should_skip_sleeping_island_solve_index(const ContactIslandGraph& graph,
                                             u32 islandIndex,
                                             const RigidBodySoA& bodies);

/// Graph-level sleep preflight for selective per-island solve batching.
IslandSleepGraphPreflight preflight_island_sleep_graph(const ContactIslandGraph& graph,
                                                       const RigidBodySoA& bodies);

/// Count islands with at least one awake dynamic body.
u32 count_awake_islands(const ContactIslandGraph& graph, const RigidBodySoA& bodies);

/// True when at least one island has awake dynamic bodies.
bool has_awake_islands(const ContactIslandGraph& graph, const RigidBodySoA& bodies);

/// Collect island indices that are not all-sleeping (parallel dispatch prep).
std::vector<u32> collect_awake_island_indices(const ContactIslandGraph& graph,
                                              const RigidBodySoA& bodies);

/// Preflight wake hint for one island when sleeping and awake dynamics coexist.
IslandWakePreflight preflight_island_wake(const ContactIslandGraph::Island& island,
                                          const RigidBodySoA& bodies);

/// Preflight wake hint by island index; out-of-range indices are marked skipped.
IslandWakePreflight preflight_island_wake_by_index(const ContactIslandGraph& graph,
                                                   u32 islandIndex,
                                                   const RigidBodySoA& bodies);

/// Early-out guard when an island has no sleeping dynamic bodies to wake.
bool should_skip_island_wake(const ContactIslandGraph::Island& island, const RigidBodySoA& bodies);

/// Wake sleeping dynamic bodies in one island; returns count awakened.
u32 wake_island_bodies(RigidBodySoA& bodies, const ContactIslandGraph::Island& island);

/// Guarded wake; returns false when the island has no sleeping dynamics to wake.
bool wake_island_bodies_guarded(RigidBodySoA& bodies, const ContactIslandGraph::Island& island);

/// Guarded wake by island index; returns false for out-of-range or no-op islands.
bool wake_island_bodies_by_index_guarded(RigidBodySoA& bodies,
                                         const ContactIslandGraph& graph,
                                         u32 islandIndex);

/// Combined constraint-ref + solve-body preflight for one island.
struct IslandSolvePreflightCombined {
    IslandConstraintRefsPreflight refs{};
    IslandSolveBodiesPreflight bodies{};
    IslandSleepPreflight sleep{};
    bool skipped = false;

    bool can_solve() const {
        return !skipped && refs.can_solve() && bodies.can_solve() && !sleep.can_skip_solve();
    }
};

/// Combined solve preflight merging constraint refs, body mobility, and sleep state.
IslandSolvePreflightCombined preflight_island_solve_combined(
    const ContactIslandGraph::Island& island,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints);

/// Guarded dispatch that skips all-sleeping and immobile islands; valid paths unchanged via `dispatch_solve_island`.
bool dispatch_solve_island_skip_sleeping(RigidBodySoA& bodies,
                                         const ContactIslandGraph& graph,
                                         u32 islandIndex,
                                         SolverWorkBuffers& workBuffers,
                                         const std::vector<DistanceConstraint>& distanceConstraints,
                                         f32 dt,
                                         f32 contactCompliance,
                                         const std::function<f32(const RigidBodySoA&, u32)>& invMassFn);

/// True when `bodyIndex` refers to a sleeping dynamic body.
bool is_sleeping_body(const RigidBodySoA& bodies, u32 bodyIndex);

/// True when `bodyIndex` refers to a static or kinematic body.
bool is_static_or_kinematic_body(const RigidBodySoA& bodies, u32 bodyIndex);

/// True when `bodyIndex` refers to a non-sleeping dynamic body.
bool is_awake_dynamic_body(const RigidBodySoA& bodies, u32 bodyIndex);

/// Preflight body index coverage for one island; sets `skipped` for empty islands.
IslandBodyRefsPreflight preflight_island_body_refs(const ContactIslandGraph::Island& island,
                                                   const RigidBodySoA& bodies);

/// Early-out guard when an island has no in-range body references to solve.
bool should_skip_island_body_refs(const ContactIslandGraph::Island& island,
                                  const RigidBodySoA& bodies);

/// Preflight sleep state for one island; sets `skipped` for empty islands.
IslandSleepPreflight preflight_island_sleep_state(const ContactIslandGraph::Island& island,
                                                  const RigidBodySoA& bodies);

/// Preflight sleep state by island index; out-of-range indices are marked skipped.
IslandSleepPreflight preflight_island_sleep_state_by_index(const ContactIslandGraph& graph,
                                                           u32 islandIndex,
                                                           const RigidBodySoA& bodies);

/// Early-out guard when all dynamic bodies in an island are sleeping.
bool should_skip_solve_sleeping_island(const ContactIslandGraph::Island& island,
                                       const RigidBodySoA& bodies);

/// Preflight wake hints for mixed sleep/awake islands; sets `skipped` for empty islands.
IslandWakePreflight preflight_island_wake(const ContactIslandGraph::Island& island,
                                          const RigidBodySoA& bodies);

/// True when an island mixes sleeping and awake dynamic bodies (wake candidates present).
bool should_wake_island(const ContactIslandGraph::Island& island, const RigidBodySoA& bodies);

/// Summarize all-sleeping vs awake islands for graph-level dispatch guards.
IslandSleepStats compute_island_sleep_stats(const ContactIslandGraph& graph,
                                            const RigidBodySoA& bodies);

/// Count islands that pass awake-dynamic solve preflight.
u32 count_awake_islands(const ContactIslandGraph& graph, const RigidBodySoA& bodies);

/// True when at least one island has an awake dynamic body to solve.
bool has_awake_islands(const ContactIslandGraph& graph, const RigidBodySoA& bodies);

/// Graph-level sleep preflight; sets `skipped` when every constrained island is all-sleeping.
IslandSleepGraphPreflight preflight_island_sleep_graph(const ContactIslandGraph& graph,
                                                       const RigidBodySoA& bodies);

/// Early-out guard for graph-level solve dispatch when no island has awake dynamics.
bool should_skip_island_dispatch_for_sleep(const ContactIslandGraph& graph,
                                           const RigidBodySoA& bodies);

/// Collect island indices with at least one awake dynamic body (parallel dispatch prep).
std::vector<u32> collect_awake_island_indices(const ContactIslandGraph& graph,
                                              const RigidBodySoA& bodies);

/// Combined solve preflight for one island pass.
IslandSolvePassPreflight preflight_island_solve_pass(
    const ContactIslandGraph::Island& island,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints);

/// Early-out guard combining constraint refs, body refs, and sleep state.
bool should_skip_island_solve_pass(const ContactIslandGraph::Island& island,
                                   const RigidBodySoA& bodies,
                                   const std::vector<narrowphase::ContactManifold>& contacts,
                                   const std::vector<DistanceConstraint>& distanceConstraints);

/// True when `bodyIndex` carries `RB_SLEEPING`.
bool is_body_sleeping(const RigidBodySoA& bodies, u32 bodyIndex);

/// True when `bodyIndex` carries `RB_STATIC` or `RB_KINEMATIC`.
bool is_body_static_or_kinematic(const RigidBodySoA& bodies, u32 bodyIndex);

/// True when every dynamic body in the island is sleeping.
bool is_island_fully_sleeping(const RigidBodySoA& bodies, const ContactIslandGraph::Island& island);

/// Preflight island sleep state; sets `skipped` for empty islands.
IslandSleepPreflight preflight_island_sleep(const RigidBodySoA& bodies,
                                            const ContactIslandGraph::Island& island);

/// Preflight island sleep by index; out-of-range indices are marked skipped.
IslandSleepPreflight preflight_island_sleep_by_index(const ContactIslandGraph& graph,
                                                     u32 islandIndex,
                                                     const RigidBodySoA& bodies);

/// Early-out guard when every dynamic body in the island is sleeping.
bool should_skip_solve_fully_sleeping_island(const RigidBodySoA& bodies,
                                             const ContactIslandGraph::Island& island);

/// Preflight island wake candidates from owned contacts; sets `skipped` for empty islands.
IslandWakePreflight preflight_island_wake(const RigidBodySoA& bodies,
                                          const ContactIslandGraph::Island& island,
                                          const std::vector<narrowphase::ContactManifold>& contacts);

/// Preflight island wake by index; out-of-range indices are marked skipped.
IslandWakePreflight preflight_island_wake_by_index(const ContactIslandGraph& graph,
                                                   u32 islandIndex,
                                                   const RigidBodySoA& bodies,
                                                   const std::vector<narrowphase::ContactManifold>& contacts);

/// Early-out guard when no sleeping bodies should wake from island contacts.
bool should_wake_island_bodies(const RigidBodySoA& bodies,
                               const ContactIslandGraph::Island& island,
                               const std::vector<narrowphase::ContactManifold>& contacts);

/// Clear `RB_SLEEPING` on island bodies that should wake; returns bodies woken.
u32 wake_island_bodies_guarded(RigidBodySoA& bodies, const ContactIslandGraph::Island& island);

/// Combined constraint-ref + sleep preflight for one island.
IslandSolveSleepPreflight preflight_solve_island_with_sleep(
    const RigidBodySoA& bodies,
    const ContactIslandGraph::Island& island,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints);

/// Early-out guard combining stale refs and fully sleeping islands.
bool should_skip_solve_island_with_sleep(const RigidBodySoA& bodies,
                                         const ContactIslandGraph::Island& island,
                                         const std::vector<narrowphase::ContactManifold>& contacts,
                                         const std::vector<DistanceConstraint>& distanceConstraints);

/// Summarize fully sleeping vs solvable islands for batch guards.
IslandSleepStats compute_island_sleep_stats(const ContactIslandGraph& graph, const RigidBodySoA& bodies);

/// Count islands that pass per-island sleep preflight for solve.
u32 count_solvable_sleep_islands(const ContactIslandGraph& graph, const RigidBodySoA& bodies);

/// True when at least one island has awake dynamic bodies to solve.
bool has_solvable_sleep_islands(const ContactIslandGraph& graph, const RigidBodySoA& bodies);

/// Summarize wakeable vs empty islands for batch guards.
IslandWakeStats compute_island_wake_stats(const ContactIslandGraph& graph,
                                          const RigidBodySoA& bodies,
                                          const std::vector<narrowphase::ContactManifold>& contacts);

/// Count islands that pass per-island wake preflight.
u32 count_wakeable_islands(const ContactIslandGraph& graph,
                           const RigidBodySoA& bodies,
                           const std::vector<narrowphase::ContactManifold>& contacts);

/// Batch wake guarded across islands with wake candidates; returns bodies woken.
u32 wake_all_islands_guarded(RigidBodySoA& bodies,
                             const ContactIslandGraph& graph,
                             const std::vector<narrowphase::ContactManifold>& contacts);

/// Guarded island solve with sleep + constraint-ref preflights; returns false when skipped.
bool solve_island_job_guarded(RigidBodySoA& bodies,
                              const ContactIslandGraph::Island& island,
                              SolverWorkBuffers& workBuffers,
                              const std::vector<DistanceConstraint>& distanceConstraints,
                              f32 dt,
                              f32 contactCompliance,
                              const std::function<f32(const RigidBodySoA&, u32)>& invMassFn);

/// True when a body carries `RB_SLEEPING`.
bool is_sleeping_body(u32 bodyFlags);

/// True when a body carries `RB_STATIC` or `RB_KINEMATIC`.
bool is_static_or_kinematic_body(u32 bodyFlags);

/// True when a body is static, kinematic, or sleeping (solver-inactive stub).
bool is_solver_inactive_body(u32 bodyFlags);

/// True when every body in the island carries `RB_SLEEPING`.
bool is_island_all_sleeping(const ContactIslandGraph::Island& island, const RigidBodySoA& bodies);

/// True when the island has at least one dynamic, non-sleeping body.
bool is_island_wakeable(const ContactIslandGraph::Island& island, const RigidBodySoA& bodies);

/// Preflight sleep/wake coverage for one island; sets `skipped` for empty islands.
IslandSleepWakePreflight preflight_island_sleep_wake(const ContactIslandGraph::Island& island,
                                                     const RigidBodySoA& bodies);

/// Preflight sleep/wake by island index; out-of-range indices are marked skipped.
IslandSleepWakePreflight preflight_island_sleep_wake_by_index(const ContactIslandGraph& graph,
                                                              u32 islandIndex,
                                                              const RigidBodySoA& bodies);

/// Early-out guard when every body in the island is sleeping.
bool should_skip_sleeping_island_solve(const ContactIslandGraph::Island& island,
                                       const RigidBodySoA& bodies);

/// Early-out guard for sleeping-island solve by index (empty, out-of-range, or all sleeping).
bool should_skip_sleeping_island_solve_index(const ContactIslandGraph& graph,
                                             u32 islandIndex,
                                             const RigidBodySoA& bodies);

/// Summarize wakeable vs all-sleeping/inactive islands for graph-level solve guards.
IslandSleepWakeStats compute_island_sleep_wake_stats(const ContactIslandGraph& graph,
                                                     const RigidBodySoA& bodies);

/// Count islands that pass per-island sleep/wake preflight.
u32 count_wakeable_islands(const ContactIslandGraph& graph, const RigidBodySoA& bodies);

/// True when at least one island has a wakeable dynamic body.
bool has_wakeable_islands(const ContactIslandGraph& graph, const RigidBodySoA& bodies);

/// Graph-level sleep/wake preflight; sets `skipped` when nothing is wakeable.
IslandSleepWakeGraphPreflight preflight_island_sleep_wake_graph(const ContactIslandGraph& graph,
                                                                const RigidBodySoA& bodies);

/// Early-out guard for graph-level solve when every constrained island is all-sleeping.
bool should_skip_sleeping_island_graph(const ContactIslandGraph& graph, const RigidBodySoA& bodies);

/// Collect island indices that pass per-island sleep/wake preflight.
std::vector<u32> collect_wakeable_island_indices(const ContactIslandGraph& graph,
                                                   const RigidBodySoA& bodies);

/// Combined constraint-ref and sleep/wake preflight for one island.
IslandConstraintSolvePreflight preflight_island_constraint_solve(
    const ContactIslandGraph::Island& island,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints);

/// Early-out guard when an island cannot solve due to stale refs or all-sleeping bodies.
bool should_skip_island_constraint_solve(const ContactIslandGraph::Island& island,
                                           const RigidBodySoA& bodies,
                                           const std::vector<narrowphase::ContactManifold>& contacts,
                                           const std::vector<DistanceConstraint>& distanceConstraints);

/// Guarded island solve with combined constraint-ref and sleep/wake preflights.
bool solve_island_job_guarded(RigidBodySoA& bodies,
                              const ContactIslandGraph::Island& island,
                              SolverWorkBuffers& workBuffers,
                              const std::vector<DistanceConstraint>& distanceConstraints,
                              f32 dt,
                              f32 contactCompliance,
                              const std::function<f32(const RigidBodySoA&, u32)>& invMassFn);

/// True when `flags` carries `RB_SLEEPING`.
bool is_body_sleeping(u32 flags);

/// True when `flags` carries `RB_STATIC` or `RB_KINEMATIC`.
bool is_body_static_or_kinematic(u32 flags);

/// True when the body is dynamic (non-static, non-kinematic).
bool is_body_dynamic(u32 flags);

/// True when a dynamic body is not sleeping.
bool is_body_dynamic_awake(u32 flags);

/// True when every dynamic body in the island carries `RB_SLEEPING`.
bool island_all_dynamic_bodies_sleeping(const ContactIslandGraph::Island& island,
                                        const RigidBodySoA& bodies);

/// True when at least one dynamic body in the island is awake.
bool island_has_awake_dynamic_body(const ContactIslandGraph::Island& island,
                                   const RigidBodySoA& bodies);

/// Preflight sleep guards for one island; sets `skipped` for empty islands.
IslandSleepSolvePreflight preflight_island_sleep_solve(const ContactIslandGraph::Island& island,
                                                       const RigidBodySoA& bodies);

/// Preflight sleep guards by island index; out-of-range indices are marked skipped.
IslandSleepSolvePreflight preflight_island_sleep_solve_by_index(const ContactIslandGraph& graph,
                                                                u32 islandIndex,
                                                                const RigidBodySoA& bodies);

/// Early-out guard when all dynamic bodies in an island are sleeping.
bool should_skip_solve_sleeping_island(const ContactIslandGraph::Island& island,
                                       const RigidBodySoA& bodies);

/// Summarize sleeping-only vs awake islands for batch guards.
IslandSleepGraphPreflight preflight_island_sleep_graph(const ContactIslandGraph& graph,
                                                       const RigidBodySoA& bodies);

/// Count islands with at least one awake dynamic body.
u32 count_awake_islands(const ContactIslandGraph& graph, const RigidBodySoA& bodies);

/// True when at least one island has an awake dynamic body.
bool has_awake_islands(const ContactIslandGraph& graph, const RigidBodySoA& bodies);

/// Early-out guard when every constrained island is sleeping-only.
bool should_skip_solve_all_sleeping_islands(const ContactIslandGraph& graph, const RigidBodySoA& bodies);

/// Preflight wake for one island; sets `skipped` for empty islands.
IslandWakePreflight preflight_island_wake(const ContactIslandGraph::Island& island,
                                         const RigidBodySoA& bodies,
                                         const std::vector<narrowphase::ContactManifold>& contacts);

/// Early-out guard when a sleeping island has no wake signal.
bool should_skip_wake_island(const ContactIslandGraph::Island& island,
                             const RigidBodySoA& bodies,
                             const std::vector<narrowphase::ContactManifold>& contacts);

/// Clear `RB_SLEEPING` and reset sleep timers for dynamic bodies in one island.
void wake_island_bodies(const ContactIslandGraph::Island& island, RigidBodySoA& bodies);

/// Guarded wake; returns false when preflight finds no wake signal.
bool wake_island_bodies_guarded(const ContactIslandGraph::Island& island,
                              RigidBodySoA& bodies,
                              const std::vector<narrowphase::ContactManifold>& contacts);

/// Combined constraint-ref + sleep preflight for one island solve pass.
IslandSolveBodyPreflight preflight_solve_island_with_bodies(
    const ContactIslandGraph::Island& island,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints);

/// Early-out guard combining constraint refs and sleep preflight.
bool should_skip_solve_island_with_bodies(const ContactIslandGraph::Island& island,
                                          const RigidBodySoA& bodies,
                                          const std::vector<narrowphase::ContactManifold>& contacts,
                                          const std::vector<DistanceConstraint>& distanceConstraints);

/// True when `bodyIndex` references a sleeping dynamic body.
bool is_sleeping_body(const RigidBodySoA& bodies, u32 bodyIndex);

/// True when a body is static, kinematic, or sleeping (no solver response).
bool is_inactive_solver_body(const RigidBodySoA& bodies, u32 bodyIndex);

/// True when both bodies are inactive for the solver.
bool is_fully_inactive_constraint_pair(const RigidBodySoA& bodies, u32 bodyA, u32 bodyB);

/// True when every body in the island carries `RB_SLEEPING`.
bool is_island_all_sleeping(const ContactIslandGraph::Island& island, const RigidBodySoA& bodies);

/// True when every body in the island is inactive for the solver.
bool is_island_all_inactive(const ContactIslandGraph::Island& island, const RigidBodySoA& bodies);

/// Preflight sleep/wake for one island; sets `skipped` for empty islands.
IslandSleepWakePreflight preflight_island_sleep_wake(const ContactIslandGraph::Island& island,
                                                     const RigidBodySoA& bodies);

/// Preflight sleep/wake by island index; out-of-range indices are marked skipped.
IslandSleepWakePreflight preflight_island_sleep_wake_by_index(const ContactIslandGraph& graph,
                                                              u32 islandIndex,
                                                              const RigidBodySoA& bodies);

/// Summarize active vs all-inactive/all-sleeping islands for batch guards.
IslandSleepWakeStats compute_island_sleep_wake_stats(const ContactIslandGraph& graph,
                                                     const RigidBodySoA& bodies);

/// Count islands that pass per-island sleep/wake preflight.
u32 count_active_islands(const ContactIslandGraph& graph, const RigidBodySoA& bodies);

/// True when at least one island has active dynamic bodies to solve.
bool has_active_islands(const ContactIslandGraph& graph, const RigidBodySoA& bodies);

/// Graph-level sleep/wake preflight; sets `skipped` when nothing is active.
IslandSleepWakeGraphPreflight preflight_island_sleep_wake_graph(const ContactIslandGraph& graph,
                                                                const RigidBodySoA& bodies);

/// Early-out guard for graph-level solve dispatch on all-inactive islands.
bool should_skip_island_sleep_wake_graph(const ContactIslandGraph& graph, const RigidBodySoA& bodies);

/// Collect island indices that pass per-island sleep/wake preflight.
std::vector<u32> collect_active_island_indices(const ContactIslandGraph& graph, const RigidBodySoA& bodies);

/// Early-out guard when an island has only inactive bodies.
bool should_skip_inactive_island_solve(const ContactIslandGraph::Island& island,
                                       const RigidBodySoA& bodies);

/// Early-out guard for inactive islands by index (empty, out-of-range, or all inactive).
bool should_skip_inactive_island_solve_index(const ContactIslandGraph& graph,
                                             u32 islandIndex,
                                             const RigidBodySoA& bodies);

/// Preflight constraint coverage with body-aware solvable counts.
IslandConstraintRefsPreflight preflight_island_solvable_constraint_refs(
    const ContactIslandGraph::Island& island,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints);

/// Early-out guard when an island has no solvable constraint references.
bool should_skip_island_solvable_constraint_refs(const ContactIslandGraph::Island& island,
                                                 const RigidBodySoA& bodies,
                                                 const std::vector<narrowphase::ContactManifold>& contacts,
                                                 const std::vector<DistanceConstraint>& distanceConstraints);

/// Combined constraint-ref and sleep/wake preflight for one island.
IslandSolveBodiesPreflight preflight_island_solve_bodies(
    const ContactIslandGraph::Island& island,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints);

/// Early-out guard combining inactive bodies and unsolvable constraint refs.
bool should_skip_island_solve_bodies(const ContactIslandGraph::Island& island,
                                     const RigidBodySoA& bodies,
                                     const std::vector<narrowphase::ContactManifold>& contacts,
                                     const std::vector<DistanceConstraint>& distanceConstraints);

/// Preflight island dispatch including timestep and sleep/wake validity.
IslandDispatchBodiesPreflight preflight_island_dispatch_with_bodies(const ContactIslandGraph& graph,
                                                                    const RigidBodySoA& bodies,
                                                                    f32 dt);

/// Early-out guard combining graph dispatch preflight and sleep/wake checks.
bool should_skip_island_dispatch_with_bodies(const ContactIslandGraph& graph,
                                             const RigidBodySoA& bodies,
                                             f32 dt);

/// True when `bodyIndex` refers to a sleeping body.
bool is_body_sleeping(const RigidBodySoA& bodies, u32 bodyIndex);

/// True when `bodyIndex` refers to a static or kinematic body.
bool is_body_static_or_kinematic(const RigidBodySoA& bodies, u32 bodyIndex);

/// Preflight sleep/wake state for one island; sets `skipped` for empty islands.
IslandSleepWakePreflight preflight_island_sleep_wake(const ContactIslandGraph::Island& island,
                                                     const RigidBodySoA& bodies);

/// Preflight sleep/wake by island index; out-of-range indices are marked skipped.
IslandSleepWakePreflight preflight_island_sleep_wake_by_index(const ContactIslandGraph& graph,
                                                              u32 islandIndex,
                                                              const RigidBodySoA& bodies);

/// Early-out guard when every dynamic body in the island is sleeping.
bool should_skip_island_solve_for_sleep(const ContactIslandGraph::Island& island,
                                        const RigidBodySoA& bodies);

/// Early-out guard for sleep/wake by island index (empty, out-of-range, or all sleeping).
bool should_skip_island_solve_for_sleep_index(const ContactIslandGraph& graph,
                                              u32 islandIndex,
                                              const RigidBodySoA& bodies);

/// Summarize solvable vs sleeping/empty islands for graph-level solve guards.
IslandSleepWakeStats compute_island_sleep_wake_stats(const ContactIslandGraph& graph,
                                                     const RigidBodySoA& bodies);

/// Count islands that pass per-island sleep/wake solve preflight.
u32 count_solvable_islands(const ContactIslandGraph& graph, const RigidBodySoA& bodies);

/// True when at least one island has awake dynamic bodies to solve.
bool has_solvable_islands(const ContactIslandGraph& graph, const RigidBodySoA& bodies);

/// Graph-level sleep/wake preflight; sets `skipped` when nothing is solvable.
IslandSleepWakeGraphPreflight preflight_island_sleep_wake_graph(const ContactIslandGraph& graph,
                                                                const RigidBodySoA& bodies);

/// Early-out guard for graph-level solve when every constrained island is fully sleeping.
bool should_skip_island_dispatch_for_sleep(const ContactIslandGraph& graph, const RigidBodySoA& bodies);

/// Collect island indices that pass per-island sleep/wake solve preflight.
std::vector<u32> collect_solvable_island_indices(const ContactIslandGraph& graph,
                                                   const RigidBodySoA& bodies);

/// Preflight one island constraint solve pass (dt + refs + sleep/wake guards).
IslandConstraintSolvePreflight preflight_island_constraint_solve(
    const ContactIslandGraph::Island& island,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints,
    f32 dt);

/// Early-out guard for one island constraint solve pass.
bool should_skip_island_constraint_solve(const ContactIslandGraph::Island& island,
                                           const RigidBodySoA& bodies,
                                           const std::vector<narrowphase::ContactManifold>& contacts,
                                           const std::vector<DistanceConstraint>& distanceConstraints,
                                           f32 dt);

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
/// Preflight one island dispatch including constraint-solve and sleep guards (B4.5 deepen follow-up pass).
IslandDispatchDeepenPreflight preflight_island_dispatch_deepen(
    const ContactIslandGraph& graph,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints,

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
/// Guarded dispatch entry that also skips fully sleeping islands.
bool dispatch_solve_island_sleep_guarded(RigidBodySoA& bodies,

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

/// Guarded dispatch combining finite-dt, constraint-ref, and sleep preflights.
bool dispatch_solve_island_combined_guarded(RigidBodySoA& bodies,
                                            const IslandSolveJob& job,
                                            SolverWorkBuffers& workBuffers,
                                            const std::vector<DistanceConstraint>& distanceConstraints,
                                            f32 dt,
                                            f32 contactCompliance,
                                            const std::function<f32(const RigidBodySoA&, u32)>& invMassFn);

/// Guarded dispatch from a pre-extracted job with explicit skip/solve outcome.
IslandDispatchResult dispatch_solve_island_job_result(RigidBodySoA& bodies,

/// Guarded dispatch that skips all-sleeping islands; does not alter awake-island solve.
IslandDispatchResult dispatch_solve_island_sleep_guarded_result(
    RigidBodySoA& bodies,
    const ContactIslandGraph& graph,
    u32 islandIndex,
    SolverWorkBuffers& workBuffers,
    const std::vector<DistanceConstraint>& distanceConstraints,
    f32 dt,
    f32 contactCompliance,
    const std::function<f32(const RigidBodySoA&, u32)>& invMassFn);

/// Batch guarded dispatch skipping all-sleeping islands; returns count actually solved.
u32 dispatch_all_awake_islands(RigidBodySoA& bodies,
                               const ContactIslandGraph& graph,
                               SolverWorkBuffers& workBuffers,
                               const std::vector<DistanceConstraint>& distanceConstraints,
                               f32 dt,
                               f32 contactCompliance,
                               const std::function<f32(const RigidBodySoA&, u32)>& invMassFn);

/// Batch guarded dispatch with explicit skip/solve counts (sleep-aware).
IslandBatchDispatchResult dispatch_all_awake_islands_result(
    RigidBodySoA& bodies,
    const ContactIslandGraph& graph,
    SolverWorkBuffers& workBuffers,
    const std::vector<DistanceConstraint>& distanceConstraints,
    f32 dt,
    f32 contactCompliance,
    const std::function<f32(const RigidBodySoA&, u32)>& invMassFn);

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

/// Guarded solve that skips stale body refs and all-sleeping islands without changing valid paths.
bool solve_island_job_sleep_guarded(RigidBodySoA& bodies,
                                    const ContactIslandGraph::Island& island,
                                    SolverWorkBuffers& workBuffers,
                                    const std::vector<DistanceConstraint>& distanceConstraints,
                                    f32 dt,
                                    f32 contactCompliance,
                                    const std::function<f32(const RigidBodySoA&, u32)>& invMassFn);

/// Guarded dispatch entry that skips all-sleeping islands; valid awake paths match `dispatch_solve_island`.
bool dispatch_solve_island_sleep_guarded(RigidBodySoA& bodies,
                                         const ContactIslandGraph& graph,
                                         u32 islandIndex,

/// Guarded dispatch with explicit skip/solve outcome and sleep preflight.
IslandDispatchResult dispatch_solve_island_sleep_guarded_result(
    RigidBodySoA& bodies,
/// Guarded island solve; returns false when constraint/sleep preflights block solve.
bool solve_island_job_guarded(RigidBodySoA& bodies,

/// Guarded island dispatch using full constraint/sleep preflights.
bool dispatch_solve_island_guarded(RigidBodySoA& bodies,

/// Guarded island dispatch from a pre-extracted job using full constraint/sleep preflights.
bool dispatch_solve_island_job_guarded(RigidBodySoA& bodies,
                                       const IslandSolveJob& job,

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

/// Non-mutating build predicate — inverse of `should_skip_island_build` (B4.4 deepen follow-up pass).
bool should_run_island_build(u32 bodyCount,
                             const std::vector<narrowphase::ContactManifold>& contacts,
                             const std::vector<DistanceConstraint>& distanceConstraints);

/// True when at least one in-range contact or distance constraint exists for build.
bool has_in_range_constraints(
    u32 bodyCount,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints);

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

/// Guarded island graph build with explicit skip/built outcome.
IslandBuildResult build_island_graph_guarded_result(ContactIslandGraph& graph,
                                                    u32 bodyCount,
                                                    const std::vector<narrowphase::ContactManifold>& contacts,
                                                    const std::vector<DistanceConstraint>& distanceConstraints);

/// Non-mutating island build skip predicate — inverse of `can_build`.
bool can_skip_island_build(u32 bodyCount,

/// Non-mutating island build predicate — mirrors guarded build eligibility.
bool should_run_island_build(u32 bodyCount,

/// Guarded build + post-build integrity check; returns false when either preflight fails.
bool build_island_graph_integrity_guarded(ContactIslandGraph& graph,
                                          u32 bodyCount,
                                          const std::vector<narrowphase::ContactManifold>& contacts,
                                          const std::vector<DistanceConstraint>& distanceConstraints);

/// Build only when island build preflight passes; no-op otherwise (B4.5 deepen follow-up pass).
bool build_island_graph_with_preflight(ContactIslandGraph& graph,
                                       u32 bodyCount,
                                       const std::vector<narrowphase::ContactManifold>& contacts,
                                       const std::vector<DistanceConstraint>& distanceConstraints);

/// Guarded island graph build with explicit build/skip outcome.
IslandBuildResult build_island_graph_result(ContactIslandGraph& graph,
                                            u32 bodyCount,
                                            const std::vector<narrowphase::ContactManifold>& contacts,
                                            const std::vector<DistanceConstraint>& distanceConstraints);

/// Guarded island graph build with explicit skip/unsafe/degenerate outcome.
IslandBuildResult build_island_graph_result(ContactIslandGraph& graph,
                                            u32 bodyCount,
                                            const std::vector<narrowphase::ContactManifold>& contacts,
                                            const std::vector<DistanceConstraint>& distanceConstraints);

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

/// Constraint-solve preflight by island index; out-of-range indices are marked skipped.
IslandConstraintSolvePreflight preflight_island_constraint_solve_by_index(
    const ContactIslandGraph& graph,
    u32 islandIndex,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints);

/// Early-out guard for constraint solve by island index (empty, out-of-range, or blocked).
bool should_skip_island_constraint_solve_by_index(const ContactIslandGraph& graph,
                                                  u32 islandIndex,
                                                  const RigidBodySoA& bodies,
                                                  const std::vector<narrowphase::ContactManifold>& contacts,
                                                  const std::vector<DistanceConstraint>& distanceConstraints);

/// Summarize solveable vs blocked islands for graph-level constraint-solve guards.
IslandConstraintSolveGraphStats compute_island_constraint_solve_stats(
    const ContactIslandGraph& graph,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints);

/// Count islands that pass per-island constraint-solve preflight.
u32 count_constraint_solveable_islands(const ContactIslandGraph& graph,
                                       const RigidBodySoA& bodies,
                                       const std::vector<narrowphase::ContactManifold>& contacts,
                                       const std::vector<DistanceConstraint>& distanceConstraints);

/// True when at least one island can run constraint solve this substep.
bool has_constraint_solveable_islands(const ContactIslandGraph& graph,
                                      const RigidBodySoA& bodies,
                                      const std::vector<narrowphase::ContactManifold>& contacts,
                                      const std::vector<DistanceConstraint>& distanceConstraints);

/// Graph-level constraint-solve preflight; sets `skipped` when nothing can solve.
IslandConstraintSolveGraphPreflight preflight_island_constraint_solve_graph(
    const ContactIslandGraph& graph,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints);

/// Early-out guard when every constrained island is blocked from constraint solve.
bool should_skip_island_constraint_solve_graph(
    const ContactIslandGraph& graph,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints);

/// Collect island indices that pass per-island constraint-solve preflight.
std::vector<u32> collect_constraint_solveable_island_indices(
    const ContactIslandGraph& graph,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints);

/// Combined wake + sleep + constraint-solve preflight for one island solve pass.
IslandSolvePipelinePreflight preflight_island_solve_pipeline(
    const ContactIslandGraph::Island& island,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints);

/// Combined wake + sleep + constraint-solve preflight by island index.
IslandSolvePipelinePreflight preflight_island_solve_pipeline_by_index(
    const ContactIslandGraph& graph,
    u32 islandIndex,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints);

/// Early-out guard when wake/sleep/refs/bodies block island solve pipeline.
bool should_skip_island_solve_pipeline(const ContactIslandGraph::Island& island,

/// Guarded island solve using full constraint-solve preflight; `solve_island_job` unchanged.
bool solve_island_job_guarded(RigidBodySoA& bodies,
                              const ContactIslandGraph::Island& island,
                              SolverWorkBuffers& workBuffers,
                              const std::vector<DistanceConstraint>& distanceConstraints,
                              f32 dt,
                              f32 contactCompliance,
                              const std::function<f32(const RigidBodySoA&, u32)>& invMassFn);

/// Wake sleepers then guarded solve when pipeline preflight allows work.
bool solve_island_job_with_wake_guarded(RigidBodySoA& bodies,

/// Guarded dispatch from a pre-extracted job using constraint-solve preflight.
bool dispatch_solve_island_job_guarded(RigidBodySoA& bodies,
                                       const IslandSolveJob& job,

/// Wake sleepers then guarded dispatch for one island index.
bool dispatch_solve_island_with_wake_guarded(RigidBodySoA& bodies,

/// Batch wake + guarded dispatch over constraint-solveable islands.
u32 dispatch_all_islands_with_wake_guarded(
    RigidBodySoA& bodies,

/// Batch wake + guarded dispatch with explicit skip/solve counts.
IslandBatchDispatchResult dispatch_all_islands_with_wake_result(

/// Combined constraint-solve preflight by island index; out-of-range indices are marked skipped.
/// Combined constraint-ref + body participation preflight by island index.
/// Constraint-solve preflight by island index; out-of-range indices are marked skipped.
IslandConstraintSolvePreflight preflight_island_constraint_solve_by_index(
    const ContactIslandGraph& graph,
    u32 islandIndex,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints);

/// Early-out guard for constraint solve by island index (empty, out-of-range, or blocked).
bool should_skip_island_constraint_solve_index(const ContactIslandGraph& graph,
                                               u32 islandIndex,
                                               const RigidBodySoA& bodies,
                                               const std::vector<narrowphase::ContactManifold>& contacts,
                                               const std::vector<DistanceConstraint>& distanceConstraints);

/// Guarded island constraint solve; returns false when preflight blocks solve.
bool solve_island_job_guarded(RigidBodySoA& bodies,
                              const ContactIslandGraph::Island& island,
                              SolverWorkBuffers& workBuffers,
                              const std::vector<DistanceConstraint>& distanceConstraints,
                              f32 dt,
                              f32 contactCompliance,
                              const std::function<f32(const RigidBodySoA&, u32)>& invMassFn);

/// Guarded island constraint solve with explicit skip/solve outcome.
IslandConstraintSolveResult solve_island_job_result(RigidBodySoA& bodies,
                                                    const ContactIslandGraph& graph,

/// Combined solve pipeline preflight for one island (dispatch + refs + bodies + sleep).
IslandSolvePipelinePreflight preflight_island_solve_pipeline(const ContactIslandGraph& graph,
                                                             f32 dt);

/// Guarded dispatch combining wake + constraint-solve preflights before solve.
bool dispatch_solve_island_with_solve_guards(RigidBodySoA& bodies,

/// Guarded dispatch with explicit skip/solve outcome and solve-pipeline preflights.
IslandDispatchResult dispatch_solve_island_with_solve_guards_result(
    RigidBodySoA& bodies,

/// Preflight constraint solve by island index; out-of-range indices are marked skipped.
IslandConstraintSolvePreflight preflight_island_constraint_solve_by_index(

/// Preflight wake + sleep + constraint solve for one island pipeline pass.
IslandSolvePipelinePreflight preflight_island_solve_pipeline(

/// Preflight solve pipeline by island index; out-of-range indices are marked skipped.
IslandSolvePipelinePreflight preflight_island_solve_pipeline_by_index(

/// Early-out guard when sleep state or constraint refs block island solve pipeline.
bool should_skip_island_solve_pipeline(const ContactIslandGraph::Island& island,

/// Guarded island solve pipeline: wake mixed sleepers then solve when preflight allows.
bool solve_island_job_pipeline_guarded(RigidBodySoA& bodies,

/// Guarded island solve pipeline by island index.
bool solve_island_job_pipeline_by_index_guarded(RigidBodySoA& bodies,

/// Collect island indices that pass constraint-solve preflight.
std::vector<u32> collect_constraint_solveable_island_indices(

/// Preflight graph dispatch including body sleep/wake participation.
IslandDispatchBodiesPreflight preflight_island_dispatch_with_bodies(const ContactIslandGraph& graph,

/// Early-out guard combining graph dispatch and body sleep preflight.
bool should_skip_island_dispatch_with_bodies(const ContactIslandGraph& graph,

/// Guarded dispatch with wake + constraint-solve pipeline preflight.
IslandDispatchResult dispatch_solve_island_pipeline_result(RigidBodySoA& bodies,

/// Batch guarded dispatch over nonsleeping islands with wake pipeline.
IslandPipelineBatchDispatchResult dispatch_all_islands_pipeline_result(

/// Batch guarded dispatch with wake + constraint-solve pipeline; returns islands solved.
u32 dispatch_all_islands_pipeline_guarded(RigidBodySoA& bodies,
/// Summarize solveable vs blocked islands for graph-level batch guards.
IslandConstraintSolveGraphStats compute_island_constraint_solve_stats(

/// Count islands that pass per-island constraint-solve preflight.
u32 count_solveable_islands(const ContactIslandGraph& graph,

/// True when at least one island can run constraint solve this substep.
bool has_solveable_islands(const ContactIslandGraph& graph,

/// Graph-level constraint-solve preflight; sets `skipped` when nothing can solve.
IslandConstraintSolveGraphPreflight preflight_island_constraint_solve_graph(

/// Collect island indices that pass per-island constraint-solve preflight.
std::vector<u32> collect_solveable_island_indices(

/// Early-out guard when refs or body participation block island constraint solve.
bool should_skip_island_constraint_solve(const ContactIslandGraph::Island& island,

/// Early-out guard for constraint solve by island index (empty or out-of-range).
bool should_skip_island_constraint_solve_by_index(const ContactIslandGraph& graph,
                                                  u32 islandIndex,
                                                  const RigidBodySoA& bodies,
                                                  const std::vector<narrowphase::ContactManifold>& contacts,
                                                  const std::vector<DistanceConstraint>& distanceConstraints);

/// Guarded island constraint solve; returns false when preflight blocks solve.
bool solve_island_job_guarded(RigidBodySoA& bodies,
                              const ContactIslandGraph::Island& island,
                              SolverWorkBuffers& workBuffers,
                              const std::vector<DistanceConstraint>& distanceConstraints,
                              f32 dt,
                              f32 contactCompliance,
                              const std::function<f32(const RigidBodySoA&, u32)>& invMassFn);

/// Guarded constraint solve by island index; returns false for out-of-range or blocked preflight.
bool dispatch_solve_island_constraint_guarded(RigidBodySoA& bodies,
                                              const ContactIslandGraph& graph,
                                              u32 islandIndex,
                                              SolverWorkBuffers& workBuffers,
                                              const std::vector<DistanceConstraint>& distanceConstraints,
                                              f32 dt,
                                              f32 contactCompliance,
                                              const std::function<f32(const RigidBodySoA&, u32)>& invMassFn);

/// Resolve constraints only when deepen preflight passes; returns false when skipped (B4.5 deepen follow-up pass).
bool solve_island_job_with_preflight(RigidBodySoA& bodies,
                                     const ContactIslandGraph::Island& island,
                                     SolverWorkBuffers& workBuffers,
                                     const std::vector<DistanceConstraint>& distanceConstraints,
                                     f32 dt,
                                     f32 contactCompliance,
                                     const std::function<f32(const RigidBodySoA&, u32)>& invMassFn);

/// Guarded dispatch with deepen preflight; skips stale refs, immovable, and all-sleeping islands (B4.5 deepen follow-up pass).
bool dispatch_solve_island_with_preflight(
    RigidBodySoA& bodies,
    const ContactIslandGraph& graph,
    u32 islandIndex,
    SolverWorkBuffers& workBuffers,
    const std::vector<DistanceConstraint>& distanceConstraints,
    f32 dt,
    f32 contactCompliance,
    const std::function<f32(const RigidBodySoA&, u32)>& invMassFn);

/// Non-mutating constraint-solve predicate — inverse of `should_skip_island_constraint_solve`.
bool should_run_island_constraint_solve(const ContactIslandGraph::Island& island,
                                        const RigidBodySoA& bodies,
                                        const std::vector<narrowphase::ContactManifold>& contacts,
                                        const std::vector<DistanceConstraint>& distanceConstraints);

/// Preflight one island job including body participation and constraint coverage.
IslandConstraintSolveJobPreflight preflight_solve_island_job_with_bodies(
    const IslandSolveJob& job,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints,
    f32 dt);

/// Early-out guard when job, refs, or body participation block island solve.
bool should_skip_solve_island_job_with_bodies(const IslandSolveJob& job,
                                              const RigidBodySoA& bodies,
                                              const std::vector<narrowphase::ContactManifold>& contacts,
                                              const std::vector<DistanceConstraint>& distanceConstraints,
                                              f32 dt);

/// Guarded island solve using full constraint + body preflight; skips all-sleeping islands.
bool solve_island_job_guarded(RigidBodySoA& bodies,
                              const ContactIslandGraph::Island& island,
                              SolverWorkBuffers& workBuffers,
                              const std::vector<DistanceConstraint>& distanceConstraints,
                              f32 dt,
                              f32 contactCompliance,
                              const std::function<f32(const RigidBodySoA&, u32)>& invMassFn);

/// Guarded dispatch with body-participation preflight and explicit skip/solve outcome.
IslandDispatchResult dispatch_solve_island_with_bodies_result(
    RigidBodySoA& bodies,
    const ContactIslandGraph& graph,
    u32 islandIndex,
    SolverWorkBuffers& workBuffers,
    const std::vector<DistanceConstraint>& distanceConstraints,
    f32 dt,
    f32 contactCompliance,
    const std::function<f32(const RigidBodySoA&, u32)>& invMassFn);

/// Collect island indices that pass constraint + body solve preflight.
std::vector<u32> collect_solveable_island_indices(
    const ContactIslandGraph& graph,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints);

/// Batch guarded dispatch skipping all-sleeping and non-movable islands.
IslandBatchBodiesDispatchResult dispatch_all_islands_with_bodies_result(
    RigidBodySoA& bodies,
    const ContactIslandGraph& graph,
    SolverWorkBuffers& workBuffers,
    const std::vector<DistanceConstraint>& distanceConstraints,
    f32 dt,
    f32 contactCompliance,
    const std::function<f32(const RigidBodySoA&, u32)>& invMassFn);

/// Constraint-solve preflight by island index; out-of-range indices are marked skipped.
IslandConstraintSolvePreflight preflight_island_constraint_solve_by_index(
    const ContactIslandGraph& graph,
    u32 islandIndex,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints);

/// Summarize solveable vs all-sleeping vs stale-ref islands for batch guards.
IslandConstraintSolveGraphStats compute_island_constraint_solve_stats(
    const ContactIslandGraph& graph,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints);

/// Graph-level constraint solve preflight; sets `skipped` when nothing can solve.
IslandConstraintSolveGraphPreflight preflight_island_constraint_solve_graph(
    const ContactIslandGraph& graph,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints);

/// Early-out guard when every constrained island fails constraint-solve preflight.
bool should_skip_island_constraint_solve_graph(
    const ContactIslandGraph& graph,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints);

/// Collect island indices that pass constraint-solve preflight (parallel dispatch prep).
std::vector<u32> collect_solveable_island_indices(
    const ContactIslandGraph& graph,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints);

/// Guarded island solve with constraint-ref and body participation preflight.
bool solve_island_job_guarded(RigidBodySoA& bodies,
                              const ContactIslandGraph::Island& island,
                              SolverWorkBuffers& workBuffers,
                              const std::vector<DistanceConstraint>& distanceConstraints,
                              f32 dt,
                              f32 contactCompliance,
                              const std::function<f32(const RigidBodySoA&, u32)>& invMassFn);

/// Guarded dispatch with constraint-solve preflight before solve.
bool dispatch_solve_island_constraint_guarded(RigidBodySoA& bodies,
                                              const ContactIslandGraph& graph,
                                              u32 islandIndex,
                                              SolverWorkBuffers& workBuffers,
                                              const std::vector<DistanceConstraint>& distanceConstraints,
                                              f32 dt,
                                              f32 contactCompliance,
                                              const std::function<f32(const RigidBodySoA&, u32)>& invMassFn);

/// Preflight sleep state for one island; sets `skipped` for empty islands.
IslandSleepPreflight preflight_island_sleep(const ContactIslandGraph::Island& island,

/// Preflight sleep state by island index; out-of-range indices are marked skipped.
IslandSleepPreflight preflight_island_sleep_by_index(const ContactIslandGraph& graph,

/// Preflight wake hints for one island; sets `skipped` for empty islands.
IslandWakePreflight preflight_island_wake(const ContactIslandGraph::Island& island,

/// Preflight wake hints by island index; out-of-range indices are marked skipped.
IslandWakePreflight preflight_island_wake_by_index(const ContactIslandGraph& graph,

/// Combined sleep + wake preflight for one island.
IslandSleepWakePreflight preflight_island_sleep_wake(const ContactIslandGraph::Island& island,
                                                     const RigidBodySoA& bodies);

/// Combined sleep + wake preflight by island index; out-of-range indices are marked skipped.
IslandSleepWakePreflight preflight_island_sleep_wake_by_index(const ContactIslandGraph& graph,
                                                              u32 islandIndex,
                                                              const RigidBodySoA& bodies);

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

/// Non-mutating wake predicate — inverse of `should_skip_island_wake` (B4.4 deepen follow-up pass).
bool should_run_island_wake(const ContactIslandGraph::Island& island, const RigidBodySoA& bodies);

/// Non-mutating graph wake predicate — inverse of `should_skip_island_wake_graph`.
bool should_run_island_wake_graph(const ContactIslandGraph& graph, const RigidBodySoA& bodies);

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

    bool should_wake() const { return !skipped && needsWake; }

/// Graph-level sleep/wake counts for batch guards (B4.4 deepen follow-up).
struct IslandSleepStats {
    u32 totalIslands = 0;
    u32 allSleepingCount = 0;
    u32 awakeCount = 0;
    u32 emptyCount = 0;
    u32 wakeRequiredCount = 0;

/// Combined constraint-solve preflight (index bounds + dt + empty island) (B4.4 deepen follow-up).
struct IslandConstraintSolvePreflight {
    u32 ownedContactCount = 0;
    u32 ownedDistanceCount = 0;
    u32 outOfRangeContactCount = 0;
    u32 outOfRangeDistanceCount = 0;
    u32 resolvableConstraintCount = 0;
    bool invalidDt = false;

    bool can_solve() const {
        return !skipped && !invalidDt && resolvableConstraintCount > 0u;
    }

/// Dispatch preflight combining graph stats, dt validity, and sleep skip (B4.4 deepen follow-up).
struct IslandDispatchSleepPreflight {
    IslandDispatchPreflight dispatch{};
    IslandSleepStats sleepStats{};
    u32 solvableCount = 0;

    bool can_dispatch() const { return !skipped && dispatch.can_dispatch() && solvableCount > 0u; }

/// Per-island solve preflight combining sleep skip and constraint bounds (B4.4 deepen follow-up).
struct IslandSolveSleepPreflight {
    IslandSleepPreflight sleep{};
    IslandWakePreflight wake{};
    IslandConstraintSolvePreflight constraints{};

        return !skipped && constraints.can_solve() && !sleep.can_skip_solve();

/// Batch dispatch summary including sleep-skipped islands (B4.4 deepen follow-up).
struct IslandBatchSleepDispatchResult {
    IslandBatchDispatchResult dispatch{};
    u32 sleepSkippedCount = 0;

IslandSleepPreflight preflight_island_sleep(const RigidBodySoA& bodies,
                                            const ContactIslandGraph::Island& island);

IslandSleepPreflight preflight_island_sleep_by_index(const RigidBodySoA& bodies,
                                                     u32 islandIndex);

IslandWakePreflight preflight_island_wake(const RigidBodySoA& bodies,

IslandWakePreflight preflight_island_wake_by_index(const RigidBodySoA& bodies,

IslandSleepStats compute_island_sleep_stats(const RigidBodySoA& bodies, const ContactIslandGraph& graph);

u32 count_all_sleeping_islands(const RigidBodySoA& bodies, const ContactIslandGraph& graph);

u32 count_wake_required_islands(const RigidBodySoA& bodies, const ContactIslandGraph& graph);

bool has_solvable_islands(const RigidBodySoA& bodies, const ContactIslandGraph& graph);

bool should_skip_island_solve_for_sleep(const RigidBodySoA& bodies,

bool should_skip_island_solve_for_sleep_index(const RigidBodySoA& bodies,

bool should_skip_island_solve_job_for_sleep(const RigidBodySoA& bodies, const IslandSolveJob& job);

IslandConstraintSolvePreflight preflight_island_constraint_solve(
    const ContactIslandGraph::Island& island,
    u32 contactManifoldCount,
    u32 distanceConstraintCount,
    f32 dt);

IslandConstraintSolvePreflight preflight_island_constraint_solve_by_index(
    u32 islandIndex,

IslandSolveSleepPreflight preflight_island_solve_sleep(const RigidBodySoA& bodies,

IslandSolveSleepPreflight preflight_island_solve_sleep_by_index(const RigidBodySoA& bodies,

IslandDispatchSleepPreflight preflight_island_dispatch_sleep(const RigidBodySoA& bodies,

bool should_skip_island_dispatch_sleep(const RigidBodySoA& bodies,

std::vector<u32> collect_solvable_island_indices(const RigidBodySoA& bodies,
                                                 const ContactIslandGraph& graph);

bool dispatch_solve_island_sleep_guarded(RigidBodySoA& bodies,
                                         const std::vector<DistanceConstraint>& distanceConstraints,
                                         f32 contactCompliance,
                                         const std::function<f32(const RigidBodySoA&, u32)>& invMassFn);

IslandDispatchResult dispatch_solve_island_sleep_result(RigidBodySoA& bodies,

u32 dispatch_all_islands_sleep_guarded(RigidBodySoA& bodies,

IslandBatchSleepDispatchResult dispatch_all_islands_sleep_result(
    RigidBodySoA& bodies,

/// Returns true when body carries `RB_SLEEPING`.
bool is_body_sleeping(u32 bodyFlags);

/// Returns true when body is static or kinematic (excluded from island sleep stats).
bool is_body_static_or_kinematic(u32 bodyFlags);

/// Returns true when all non-static, non-kinematic bodies in the island are sleeping.
bool is_island_all_sleeping(const ContactIslandGraph::Island& island, const RigidBodySoA& bodies);

/// Returns true when the island has at least one awake dynamic body.
bool island_has_awake_dynamic_body(const ContactIslandGraph::Island& island, const RigidBodySoA& bodies);

/// Preflight per-island sleep state for constraint-solve guards.
IslandSleepPreflight preflight_island_sleep(const ContactIslandGraph::Island& island,
                                            const RigidBodySoA& bodies);

/// Preflight per-island sleep state by island index; out-of-range indices are marked skipped.
IslandSleepPreflight preflight_island_sleep_by_index(const ContactIslandGraph& graph,

/// Summarize all-sleeping vs awake islands for batch solve guards.
IslandSleepStats compute_island_sleep_stats(const ContactIslandGraph& graph, const RigidBodySoA& bodies);

/// Count islands with at least one awake dynamic body.
u32 count_awake_islands(const ContactIslandGraph& graph, const RigidBodySoA& bodies);

/// True when at least one island has an awake dynamic body.
bool has_awake_islands(const ContactIslandGraph& graph, const RigidBodySoA& bodies);

/// Graph-level sleep preflight; sets `skipped` when every island is empty or all-sleeping.
IslandSleepGraphPreflight preflight_island_sleep_graph(const ContactIslandGraph& graph,

/// Early-out guard for graph-level awake-only dispatch.
bool should_skip_awake_island_dispatch(const ContactIslandGraph& graph, const RigidBodySoA& bodies);

/// Collect island indices with at least one awake dynamic body.
std::vector<u32> collect_awake_island_indices(const ContactIslandGraph& graph, const RigidBodySoA& bodies);

/// Early-out guard for per-island solve when all dynamic bodies are sleeping.
bool should_skip_sleeping_island_solve(const ContactIslandGraph::Island& island, const RigidBodySoA& bodies);

/// Early-out guard for sleeping-island solve by index (empty, out-of-range, or all-sleeping).
bool should_skip_sleeping_island_solve_index(const ContactIslandGraph& graph,

/// Preflight per-island wake candidates from forces, impulses, and awake neighbors.
IslandWakePreflight preflight_island_wake(const ContactIslandGraph::Island& island,
                                          const RigidBodySoA& bodies,
                                          const std::vector<narrowphase::ContactManifold>& contacts);

/// Preflight per-island wake by island index; out-of-range indices are marked skipped.
IslandWakePreflight preflight_island_wake_by_index(const ContactIslandGraph& graph,

/// Early-out guard for wake dispatch on empty islands.
bool should_skip_island_wake(const ContactIslandGraph::Island& island);

/// True when the island has sleeping bodies that should wake this substep.
bool should_wake_island(const ContactIslandGraph::Island& island,

/// Clear `RB_SLEEPING` on wake candidates in one island; returns bodies woken.
u32 wake_island_bodies_guarded(RigidBodySoA& bodies,

/// Clear `RB_SLEEPING` on wake candidates by island index; returns bodies woken.
u32 wake_island_bodies_by_index_guarded(RigidBodySoA& bodies,

/// Preflight constraint solve for one island (empty, dt, sleep, and index guards).
IslandConstraintSolvePreflight preflight_solve_island(const ContactIslandGraph::Island& island,
                                                      const SolverWorkBuffers& workBuffers,

/// Preflight constraint solve by island index; out-of-range indices are marked skipped.
IslandConstraintSolvePreflight preflight_solve_island_by_index(const ContactIslandGraph& graph,

/// Early-out guard combining constraint-solve preflight checks.
bool should_skip_solve_island_preflight(const ContactIslandGraph::Island& island,

/// Guarded island solve using constraint-solve preflight; skips empty, invalid-dt, and all-sleeping islands.
bool solve_island_job_guarded(RigidBodySoA& bodies,

/// Guarded dispatch that skips all-sleeping islands in addition to existing dispatch guards.
bool dispatch_solve_awake_island(RigidBodySoA& bodies,

/// Guarded dispatch from a pre-extracted job with awake-only sleep guard.
bool dispatch_solve_awake_island_job(RigidBodySoA& bodies,
                                     const IslandSolveJob& job,

/// Per-island sleep/wake body counts for solve and sleep-detection preflights (B4.4 deepen follow-up).
    u32 totalBodies = 0;
    u32 sleepingCount = 0;
    u32 staticOrKinematicCount = 0;

/// Preflight for skipping island constraint solve when all dynamic bodies are sleeping.
struct IslandSleepSolvePreflight {
    IslandSleepStats stats{};
    bool allDynamicSleeping = false;

    bool can_solve() const { return !skipped && !allDynamicSleeping; }

/// Preflight for island sleep/wake threshold checks (parallel sleep-detection stub).
    u32 eligibleDynamicCount = 0;
    u32 belowThresholdCount = 0;
    u32 aboveThresholdCount = 0;

    bool can_enter_sleep() const { return !skipped && belowThresholdCount > 0u; }
    bool needs_wake() const { return !skipped && aboveThresholdCount > 0u; }

/// Combined constraint-solve preflight (dt + empty island + body-index + sleep guards).
struct IslandSolveJobPreflight {
    bool emptyIsland = false;
    bool outOfRangeBodyIndices = false;
    IslandSleepSolvePreflight sleep{};

        return !skipped && !invalidDt && !emptyIsland && !outOfRangeBodyIndices && sleep.can_solve();

/// Explicit solve outcome for guarded job entry points.
struct IslandSolveJobResult {
    bool solved = false;
    u32 islandIndex = ContactIslandGraph::invalidIsland;

/// True when `bodyIndex` references a sleeping body.
bool is_sleeping_body(const RigidBodySoA& bodies, u32 bodyIndex);

/// True when `bodyIndex` references a static or kinematic body.
bool is_static_or_kinematic_body(const RigidBodySoA& bodies, u32 bodyIndex);

/// True when every body index in the island is in range for `bodyCount`.
bool island_body_indices_valid(const ContactIslandGraph::Island& island, u32 bodyCount);

/// True when the island carries at least one out-of-range body index.
bool has_out_of_range_island_body_indices(const ContactIslandGraph::Island& island, u32 bodyCount);

/// Summarize sleeping vs awake bodies owned by one island.
IslandSleepStats compute_island_sleep_stats(const ContactIslandGraph::Island& island,

/// True when every non-static, non-kinematic body in the island is sleeping.
bool island_all_dynamic_bodies_sleeping(const ContactIslandGraph::Island& island,

/// True when the island owns at least one awake dynamic body.
bool island_has_awake_dynamic_body(const ContactIslandGraph::Island& island,

/// Preflight island constraint solve for sleeping bodies; empty islands are marked skipped.
IslandSleepSolvePreflight preflight_island_sleep_solve(const ContactIslandGraph::Island& island,

/// Preflight island constraint solve by index; out-of-range indices are marked skipped.
IslandSleepSolvePreflight preflight_island_sleep_solve_by_index(const ContactIslandGraph& graph,

/// Early-out guard for solving islands whose dynamic bodies are all sleeping.
bool should_skip_sleeping_island_solve(const ContactIslandGraph::Island& island,

/// Early-out guard for sleeping-island solve by index (empty, out-of-range, or all sleeping).

/// Preflight island sleep/wake thresholds for parallel sleep-detection stubs.
                                          f32 linearThreshold,
                                          f32 angularThreshold);

/// Preflight island sleep/wake by index; out-of-range indices are marked skipped.

/// Early-out guard when no dynamic bodies in the island are eligible for sleep detection.
bool should_skip_island_sleep_detection(const ContactIslandGraph::Island& island,

/// Early-out guard for sleep detection by island index (empty or out-of-range).
bool should_skip_island_sleep_detection_index(const ContactIslandGraph& graph,

/// Preflight one island constraint-solve job (dt, empty, body-index, and sleep guards).
IslandSolveJobPreflight preflight_solve_island_job(const ContactIslandGraph::Island& island,

/// Preflight constraint-solve job by island index; out-of-range indices are marked skipped.
IslandSolveJobPreflight preflight_solve_island_job_by_index(const ContactIslandGraph& graph,

/// Early-out guard combining dt, empty-island, body-index, and sleeping-island checks.
bool should_skip_solve_island_job_preflight(const ContactIslandGraph::Island& island,

/// Guarded constraint solve; returns false when preflight rejects the island job.

/// Guarded constraint solve with explicit skip/solve outcome.
IslandSolveJobResult solve_island_job_result(RigidBodySoA& bodies,

/// True when a body carries `RB_STATIC` or `RB_KINEMATIC`.


/// True when a dynamic body is not sleeping.
bool is_awake_dynamic_body(const RigidBodySoA& bodies, u32 bodyIndex);

/// True when every non-static/kinematic body in the island is sleeping.
bool island_all_dynamic_bodies_sleeping(const RigidBodySoA& bodies, const ContactIslandGraph::Island& island);

/// True when at least one non-static/kinematic body in the island is awake.
bool island_has_awake_dynamic_bodies(const RigidBodySoA& bodies, const ContactIslandGraph::Island& island);

/// True when a sleeping body has non-zero force/torque or velocity above `velocityThreshold`.
bool body_has_wake_impetus(const RigidBodySoA& bodies, u32 bodyIndex, f32 velocityThreshold = 0.f);

/// Preflight sleep state for one island; sets `skipped` for empty islands.
IslandSleepPreflight preflight_island_sleep(const RigidBodySoA& bodies, const ContactIslandGraph::Island& island);

/// Preflight sleep state by island index; out-of-range indices are marked skipped.

/// Summarize all-sleeping vs has-awake islands for batch guards.

/// Count islands where every dynamic body is sleeping.

bool has_awake_islands(const RigidBodySoA& bodies, const ContactIslandGraph& graph);

/// Graph-level sleep preflight; sets `skipped` when no islands exist.
IslandSleepGraphPreflight preflight_island_sleep_graph(const RigidBodySoA& bodies,

/// Early-out guard for constraint solve when every dynamic body in the island is sleeping.
bool should_skip_island_solve_sleeping(const RigidBodySoA& bodies, const ContactIslandGraph::Island& island);

/// Early-out guard for constraint solve by island index (empty, out-of-range, or all-sleeping).
bool should_skip_island_solve_sleeping_index(const ContactIslandGraph& graph,

std::vector<u32> collect_awake_dispatchable_island_indices(const RigidBodySoA& bodies,

/// Preflight wake candidates among sleeping bodies in one island.
                                          f32 velocityThreshold = 0.f);

/// Preflight wake candidates by island index; out-of-range indices are marked skipped.

/// Early-out guard for wake preflight on empty islands.
bool should_skip_island_wake_check(const ContactIslandGraph::Island& island);

/// Combined constraint-solve preflight including sleep and timestep guards.
IslandConstraintSolvePreflight preflight_island_constraint_solve(const RigidBodySoA& bodies,

/// Early-out guard combining dispatch, sleep, and timestep validity.
bool should_skip_island_constraint_solve(const RigidBodySoA& bodies,

/// Guarded dispatch that skips all-sleeping islands in addition to empty/out-of-range jobs.

/// Guarded dispatch with explicit skip/solve outcome including sleep guard.
IslandDispatchResult dispatch_solve_island_sleep_guarded_result(

/// Batch guarded dispatch over awake islands only; returns count of islands actually solved.
u32 dispatch_awake_islands(RigidBodySoA& bodies,

/// Batch guarded dispatch over awake islands with explicit skip/solve counts.
IslandBatchDispatchResult dispatch_awake_islands_result(

/// Per-island sleep/wake body counts for solve and wake preflight (B4.4 deepen follow-up).
    u32 activeDynamicCount = 0;

/// Preflight sleep state for one island before constraint solve dispatch.
    bool allStaticOrSleeping = false;

    bool can_solve() const { return !skipped && !allStaticOrSleeping; }

/// Preflight wake candidates for sleeping bodies in one island.
    u32 forceWakeCount = 0;
    u32 contactWakeCount = 0;

    bool can_wake() const { return !skipped && (forceWakeCount > 0u || contactWakeCount > 0u); }

/// Combined sleep + constraint + timestep preflight for one island solve pass.
    u32 ownedConstraintCount = 0;

        return !skipped && !invalidDt && sleep.can_solve() && resolvableConstraintCount > 0u;

bool body_is_sleeping(const RigidBodySoA& bodies, u32 bodyIndex);

bool body_is_static_or_kinematic(const RigidBodySoA& bodies, u32 bodyIndex);

/// True when `bodyIndex` references a non-static, non-kinematic, non-sleeping body.
bool body_is_active_dynamic(const RigidBodySoA& bodies, u32 bodyIndex);

/// Effective inverse mass for constraint solve preflight (matches PBD solver stub).
f32 island_effective_inv_mass(const RigidBodySoA& bodies, u32 bodyIndex);

/// Summarize sleep/active/static body counts for one island.
IslandSleepStats compute_island_sleep_stats(const RigidBodySoA& bodies,



/// True when every dynamic body in the island is sleeping.

/// Early-out guard for island solve when all bodies are static or sleeping.
bool should_skip_solve_sleeping_island(const RigidBodySoA& bodies,

/// Preflight wake candidates for one island; sets `skipped` for empty islands.


/// Early-out guard for island wake when no sleeping bodies need activation.
bool should_skip_island_wake(const RigidBodySoA& bodies, const ContactIslandGraph::Island& island);

/// True when at least one sleeping body in the island should wake this frame.
bool should_wake_island(const RigidBodySoA& bodies, const ContactIslandGraph::Island& island);

/// Guarded wake entry: clears `RB_SLEEPING` on force/contact wake candidates; returns bodies woken.
u32 wake_island_bodies_guarded(RigidBodySoA& bodies, const ContactIslandGraph::Island& island);

/// Guarded wake by island index; returns false for out-of-range or empty islands.

/// Count constraints in one island that can move at least one non-sleeping dynamic body.
u32 count_resolvable_island_constraints(const RigidBodySoA& bodies,

/// Preflight one island constraint solve pass including sleep and dt guards.


/// Early-out guard combining sleep, empty-island, and resolvable-constraint checks.

/// Guarded island solve entry: skips empty, fully static/sleeping, and invalid-dt islands.

/// Per-island sleep/wake threshold bundle (mirrors `SolverParams` sleep fields).
struct IslandSleepParams {
    f32 linearThreshold = 0.01f;
    f32 angularThreshold = 0.01f;
    f32 timeRequired = 0.5f;

/// Per-island body-state counts for constraint-solve preflight (B4.4 deepen follow-up).
struct IslandSolveBodyStats {
    u32 dynamicCount = 0;

/// Constraint-solve preflight including sleeping-body guards (B4.4 deepen follow-up).
struct IslandSolveBodyPreflight {
    IslandSolveBodyStats stats{};
    bool allStaticOrKinematic = false;

        return !skipped && !allSleeping && stats.awakeDynamicCount > 0u;

/// Per-island sleep eligibility diagnostics (B4.4 deepen follow-up).
    u32 alreadySleepingCount = 0;

    bool can_sleep() const {
        return !skipped && !invalidDt && !allStaticOrKinematic && dynamicCount > 0u &&
               belowThresholdCount == dynamicCount && alreadySleepingCount < dynamicCount;

/// Per-island wake diagnostics (B4.4 deepen follow-up).

    bool should_wake() const { return !skipped && (aboveThresholdCount > 0u || sleepingCount > 0u); }

/// Graph-level sleep batch summary (B4.4 deepen follow-up).
struct IslandSleepGraphStats {
    u32 sleepableCount = 0;
    u32 staticOnlyCount = 0;

/// Graph-level sleep preflight for batch guards (B4.4 deepen follow-up).
struct IslandSleepGraphPreflight {
    IslandSleepGraphStats stats{};

    bool can_sleep_any() const { return !skipped && !invalidDt && stats.sleepableCount > 0u; }

/// Graph-level wake batch summary (B4.4 deepen follow-up).
struct IslandWakeGraphStats {
    u32 wakeableCount = 0;

/// Graph-level wake preflight for batch guards (B4.4 deepen follow-up).
struct IslandWakeGraphPreflight {
    IslandWakeGraphStats stats{};

    bool should_wake_any() const { return !skipped && stats.wakeableCount > 0u; }

/// True when body flags include `RB_STATIC` or `RB_KINEMATIC`.
bool is_static_or_kinematic_body(u32 flags);

/// True when body flags include `RB_SLEEPING`.
bool is_sleeping_body(u32 flags);

/// True when linear/angular speeds are below sleep thresholds.
bool is_below_sleep_threshold(const RigidBodySoA& bodies, u32 bodyIndex, const IslandSleepParams& params);

/// True when linear/angular speeds exceed sleep thresholds (wake stub).
bool is_above_wake_threshold(const RigidBodySoA& bodies, u32 bodyIndex, const IslandSleepParams& params);

/// Summarize per-island body states for solve/sleep guards.
IslandSolveBodyStats compute_island_body_stats(const RigidBodySoA& bodies,

/// True when every dynamic body in the island carries `RB_SLEEPING`.
bool is_island_all_sleeping(const RigidBodySoA& bodies, const ContactIslandGraph::Island& island);

/// True when every body in the island is static or kinematic.
bool is_island_all_static_or_kinematic(const RigidBodySoA& bodies,

/// Preflight constraint solve for one island including sleeping-body guards.
IslandSolveBodyPreflight preflight_island_solve_bodies(const RigidBodySoA& bodies,

IslandSolveBodyPreflight preflight_island_solve_bodies_by_index(const RigidBodySoA& bodies,

/// Early-out guard for island solve when all dynamic bodies are sleeping.

/// Early-out guard for island solve by index (sleeping or out-of-range).

/// True when a job should run constraint solve given body sleep state.
bool should_solve_island_with_bodies(const IslandSolveJob& job,

/// Inverse of `should_solve_island_with_bodies`.
bool should_skip_island_solve_job_for_sleep(const IslandSolveJob& job,

/// Guarded dispatch including sleeping-body preflight; skips all-sleeping islands.
bool dispatch_solve_island_with_body_guards(RigidBodySoA& bodies,

/// Guarded dispatch with explicit skip/solve outcome and sleeping-body preflight.
IslandDispatchResult dispatch_solve_island_with_body_guards_result(

/// Preflight per-island sleep eligibility; sets `skipped` for empty islands.
                                            const IslandSleepParams& params,

/// Preflight per-island sleep by island index; out-of-range indices are marked skipped.

/// Preflight per-island wake eligibility; sets `skipped` for empty islands.
                                          const IslandSleepParams& params);


/// Early-out guard for per-island sleep on empty islands or invalid dt.
bool should_skip_island_sleep(const ContactIslandGraph::Island& island, f32 dt);

/// Early-out guard for per-island sleep by island index.
bool should_skip_island_sleep_index(const ContactIslandGraph& graph, u32 islandIndex, f32 dt);

/// Early-out guard for per-island wake on empty islands.

/// Early-out guard for per-island wake by island index.
bool should_skip_island_wake_index(const ContactIslandGraph& graph, u32 islandIndex);

/// Summarize sleepable vs empty/static islands for graph-level batch guards.
IslandSleepGraphStats compute_island_sleep_graph_stats(const RigidBodySoA& bodies,

/// Graph-level sleep preflight; sets `skipped` when nothing can sleep.

/// Summarize wakeable islands for graph-level batch guards.
IslandWakeGraphStats compute_island_wake_graph_stats(const RigidBodySoA& bodies,

/// Graph-level wake preflight; sets `skipped` when nothing should wake.
IslandWakeGraphPreflight preflight_island_wake_graph(const RigidBodySoA& bodies,

/// Collect island indices that pass per-island sleep preflight.
std::vector<u32> collect_sleepable_island_indices(const RigidBodySoA& bodies,

/// Collect island indices that pass per-island wake preflight.
std::vector<u32> collect_wakeable_island_indices(const RigidBodySoA& bodies,

/// Guarded per-island sleep; returns false for empty, static-only, or active islands.
bool sleep_island_bodies_guarded(RigidBodySoA& bodies,

/// Guarded per-island wake; returns false for empty islands with no sleeping bodies.
bool wake_island_bodies_guarded(RigidBodySoA& bodies, const ContactIslandGraph::Island& island);

/// Guarded per-island sleep by island index.
bool sleep_island_bodies_by_index_guarded(RigidBodySoA& bodies,

/// Guarded per-island wake by island index.
bool wake_island_bodies_by_index_guarded(RigidBodySoA& bodies,

/// Batch guarded sleep over sleepable islands; returns count of islands put to sleep.
u32 sleep_all_islands_guarded(RigidBodySoA& bodies,

/// Batch guarded wake over wakeable islands; returns count of islands woken.
u32 wake_all_islands_guarded(RigidBodySoA& bodies,








































































































/// True when body flags carry `RB_SLEEPING` (B4.4 deepen).
bool is_island_body_sleeping(u32 bodyFlags);

/// True when body flags carry `RB_STATIC` or `RB_KINEMATIC` (B4.4 deepen).
bool is_island_body_static_or_kinematic(u32 bodyFlags);

/// Per-island sleep eligibility preflight (B4.4 deepen).

    bool all_dynamic_sleeping() const {
        return !skipped && awakeDynamicCount == 0u && sleepingCount > 0u;

    bool can_attempt_sleep() const { return !skipped && awakeDynamicCount > 0u; }

/// Per-island wake preflight when sleeping bodies share constraints (B4.4 deepen).

    bool should_wake() const {
        return !skipped && sleepingBodyCount > 0u &&
               (awakeBodyCount > 0u || ownedConstraintCount > 0u);

/// Aggregate sleep/wake counts for graph-level batch guards (B4.4 deepen).
struct IslandSleepWakeStats {
    u32 wakeCandidateCount = 0;

/// Graph-level sleep/wake preflight for selective per-island sleep batching (B4.4 deepen).
struct IslandSleepWakeGraphPreflight {
    IslandSleepWakeStats stats{};

    bool has_wake_candidates() const { return !skipped && stats.wakeCandidateCount > 0u; }
    bool all_islands_sleeping() const {
        return !skipped && stats.totalIslands > 0u && stats.allSleepingCount == stats.totalIslands;

/// Constraint participation coverage for one island solve pass (sleep/static guards, B4.4 deepen).
struct IslandSolveParticipationPreflight {
    u32 ownedBodyCount = 0;
    u32 participatingBodyCount = 0;
    u32 staticOrKinematicBodyCount = 0;

    bool can_solve() const { return !skipped && participatingBodyCount > 0u; }

/// Preflight per-island sleep eligibility from body flags (B4.4 deepen).

/// Preflight per-island wake when sleeping and awake bodies share an island (B4.4 deepen).

/// Preflight sleep by island index; out-of-range indices are marked skipped (B4.4 deepen).

/// Preflight wake by island index; out-of-range indices are marked skipped (B4.4 deepen).

/// Summarize all-sleeping vs wake-candidate islands for graph-level guards (B4.4 deepen).
IslandSleepWakeStats compute_island_sleep_wake_stats(const ContactIslandGraph& graph,

/// Graph-level sleep/wake preflight; sets `skipped` when the graph is empty (B4.4 deepen).
IslandSleepWakeGraphPreflight preflight_island_sleep_wake_graph(const ContactIslandGraph& graph,

/// Early-out guard for graph-level wake batching (B4.4 deepen).
bool should_skip_island_wake_graph(const ContactIslandGraph& graph, const RigidBodySoA& bodies);

/// Collect island indices that pass per-island wake preflight (B4.4 deepen).
std::vector<u32> collect_wake_candidate_island_indices(const ContactIslandGraph& graph,

/// Early-out guard when every dynamic body in an island is sleeping (B4.4 deepen).
bool should_skip_island_solve_all_sleeping(const ContactIslandGraph::Island& island,

/// Early-out guard when an island has no participating dynamic bodies for constraint solve (B4.4 deepen).
bool should_skip_island_solve_no_participation(const ContactIslandGraph::Island& island,

/// Preflight constraint participation for one island; sets `skipped` for empty islands (B4.4 deepen).
IslandSolveParticipationPreflight preflight_island_solve_participation(

/// Guarded wake with explicit skip/activate outcome and sleeper count.
IslandWakeResult wake_island_sleepers_result(RigidBodySoA& bodies, const ContactIslandGraph::Island& island);

/// Guarded wake by island index with explicit skip/activate outcome.
IslandWakeResult wake_island_sleepers_by_index_result(RigidBodySoA& bodies,
                                                      const ContactIslandGraph& graph,
                                                      u32 islandIndex);

/// Batch guarded wake with explicit skip/activate counts.
IslandWakeBatchResult wake_all_island_sleepers_result(RigidBodySoA& bodies, const ContactIslandGraph& graph);

/// Collect island indices that are dispatchable and not all-sleeping (parallel solve prep).
std::vector<u32> collect_solveable_island_indices(const ContactIslandGraph& graph, const RigidBodySoA& bodies);

/// Batch guarded dispatch skipping all-sleeping islands; returns count actually solved.
u32 dispatch_all_islands_skipping_sleepers(RigidBodySoA& bodies,
                                           const ContactIslandGraph& graph,
                                           SolverWorkBuffers& workBuffers,
                                           const std::vector<DistanceConstraint>& distanceConstraints,
                                           f32 dt,
                                           f32 contactCompliance,
                                           const std::function<f32(const RigidBodySoA&, u32)>& invMassFn);

/// Batch guarded dispatch skipping all-sleeping islands with explicit skip/solve counts.
IslandBatchDispatchResult dispatch_all_islands_skipping_sleepers_result(
    RigidBodySoA& bodies,
    const ContactIslandGraph& graph,
    SolverWorkBuffers& workBuffers,
    const std::vector<DistanceConstraint>& distanceConstraints,
    f32 dt,
    f32 contactCompliance,
    const std::function<f32(const RigidBodySoA&, u32)>& invMassFn);

/// Guarded wake with explicit skip/activate outcome.
IslandWakeResult wake_island_sleepers_result(RigidBodySoA& bodies,
                                             const ContactIslandGraph& graph,
                                             u32 islandIndex);

/// Batch guarded wake with explicit skip/activate counts.
IslandBatchWakeResult wake_all_island_sleepers_result(RigidBodySoA& bodies,
                                                      const ContactIslandGraph& graph);

/// Preflight island dispatch including sleep-state guards.
IslandSleepAwareDispatchPreflight preflight_island_sleep_aware_dispatch(const ContactIslandGraph& graph,
                                                                        const RigidBodySoA& bodies,
                                                                        f32 dt);

/// Early-out guard combining graph dispatch and sleep-state preflight.
bool should_skip_island_sleep_aware_dispatch(const ContactIslandGraph& graph,
                                             const RigidBodySoA& bodies,
                                             f32 dt);

/// Collect island indices that pass constraint-solve preflight (not all-sleeping, in-range refs).
std::vector<u32> collect_constraint_solveable_island_indices(
    const ContactIslandGraph& graph,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints);

/// Guarded wake-then-solve for one island; wakes mixed sleepers before constraint solve.
bool dispatch_solve_island_with_wake_guarded(RigidBodySoA& bodies,
                                             const ContactIslandGraph& graph,
                                             u32 islandIndex,
                                             SolverWorkBuffers& workBuffers,
                                             const std::vector<DistanceConstraint>& distanceConstraints,
                                             f32 dt,
                                             f32 contactCompliance,
                                             const std::function<f32(const RigidBodySoA&, u32)>& invMassFn);

/// Batch guarded dispatch that wakes mixed islands and skips all-sleeping islands.
IslandSleepAwareBatchDispatchResult dispatch_all_islands_sleep_aware_result(
    RigidBodySoA& bodies,
    const ContactIslandGraph& graph,
    SolverWorkBuffers& workBuffers,
    const std::vector<DistanceConstraint>& distanceConstraints,
    f32 dt,
    f32 contactCompliance,
    const std::function<f32(const RigidBodySoA&, u32)>& invMassFn);

/// Batch guarded sleep-aware dispatch; returns count of islands actually solved.
u32 dispatch_all_islands_sleep_aware_guarded(RigidBodySoA& bodies,
                                             const ContactIslandGraph& graph,
                                             SolverWorkBuffers& workBuffers,
                                             const std::vector<DistanceConstraint>& distanceConstraints,
                                             f32 dt,
                                             f32 contactCompliance,
                                             const std::function<f32(const RigidBodySoA&, u32)>& invMassFn);

/// Preflight wake-then-solve for one island; sets `skipped` for empty islands.
IslandWakeAndSolvePreflight preflight_island_wake_and_solve(
    const ContactIslandGraph::Island& island,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints);

/// Preflight wake-then-solve by island index; out-of-range indices are marked skipped.
IslandWakeAndSolvePreflight preflight_island_wake_and_solve_by_index(
    const ContactIslandGraph& graph,
    u32 islandIndex,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints);

/// Early-out guard when wake-then-solve preflight blocks island dispatch.
bool should_skip_island_wake_and_solve(const ContactIslandGraph::Island& island,
                                       const RigidBodySoA& bodies,
                                       const std::vector<narrowphase::ContactManifold>& contacts,
                                       const std::vector<DistanceConstraint>& distanceConstraints);

/// Graph-level wake-then-solve preflight; sets `skipped` when nothing is dispatchable.
IslandWakeAndSolveGraphPreflight preflight_island_wake_and_solve_graph(const ContactIslandGraph& graph,
                                                                       const RigidBodySoA& bodies,
                                                                       f32 dt);

/// Early-out guard for graph-level wake-then-solve batching.
bool should_skip_island_wake_and_solve_graph(const ContactIslandGraph& graph,
                                             const RigidBodySoA& bodies,
                                             f32 dt);

/// Collect island indices that pass wake-then-solve preflight (parallel solve prep stub).
std::vector<u32> collect_solveable_island_indices(const ContactIslandGraph& graph,
                                                  const RigidBodySoA& bodies,
                                                  const std::vector<narrowphase::ContactManifold>& contacts,
                                                  const std::vector<DistanceConstraint>& distanceConstraints);

/// Guarded wake-then-solve for one island; wakes sleepers when needed, then solves.
IslandWakeAndSolveResult dispatch_solve_island_with_wake_result(
    RigidBodySoA& bodies,
    const ContactIslandGraph& graph,
    u32 islandIndex,
    SolverWorkBuffers& workBuffers,
    const std::vector<DistanceConstraint>& distanceConstraints,
    f32 dt,
    f32 contactCompliance,
    const std::function<f32(const RigidBodySoA&, u32)>& invMassFn);

/// Guarded wake-then-solve for one island; returns false when preflight blocks dispatch.
bool dispatch_solve_island_with_wake_guarded(RigidBodySoA& bodies,
                                             const ContactIslandGraph& graph,
                                             u32 islandIndex,
                                             SolverWorkBuffers& workBuffers,
                                             const std::vector<DistanceConstraint>& distanceConstraints,
                                             f32 dt,
                                             f32 contactCompliance,
                                             const std::function<f32(const RigidBodySoA&, u32)>& invMassFn);

/// Batch guarded dispatch over nonsleeping islands with explicit skip/solve counts.
IslandBatchDispatchResult dispatch_all_nonsleeping_islands_result(
    RigidBodySoA& bodies,
    const ContactIslandGraph& graph,
    SolverWorkBuffers& workBuffers,
    const std::vector<DistanceConstraint>& distanceConstraints,
    f32 dt,
    f32 contactCompliance,
    const std::function<f32(const RigidBodySoA&, u32)>& invMassFn);

/// Batch guarded wake-then-solve with explicit woke/solved/skipped counts.
IslandBatchWakeAndSolveResult dispatch_all_islands_with_wake_result(
    RigidBodySoA& bodies,
    const ContactIslandGraph& graph,
    SolverWorkBuffers& workBuffers,
    const std::vector<DistanceConstraint>& distanceConstraints,
    f32 dt,
    f32 contactCompliance,
    const std::function<f32(const RigidBodySoA&, u32)>& invMassFn);

/// Guarded wake with explicit skip/activate outcome.
IslandWakeResult wake_island_sleepers_result(RigidBodySoA& bodies,
                                             const ContactIslandGraph& graph,
                                             u32 islandIndex);

/// Batch guarded wake with explicit skip/activate counts.
IslandBatchWakeResult wake_all_island_sleepers_result(RigidBodySoA& bodies,
                                                      const ContactIslandGraph& graph);

/// Why island graph build would early-out (B4.4 deepen follow-up).
enum class IslandBuildRejectReason : u8 {
    None = 0,
    EmptyInputs,
    OutOfRangeContactBodies,
    OutOfRangeDistanceBodies,
};

/// Human-readable label for island build reject reasons (B4.4 deepen follow-up).
const char* island_build_reject_reason_name(IslandBuildRejectReason reason);

/// Diagnose why island graph build would skip (B4.4 deepen follow-up).
IslandBuildRejectReason island_build_reject_reason(
    u32 bodyCount,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints);

/// Returns true when `island_build_reject_reason` matches `expected` (B4.4 deepen follow-up).
bool island_build_rejects_for_reason(
    u32 bodyCount,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints,
    IslandBuildRejectReason expected);

/// Const preflight for island graph build with reject reason (B4.4 deepen follow-up).
struct IslandBuildDeepenPreflight {
    IslandBuildRejectReason reason = IslandBuildRejectReason::None;
    IslandBuildStats stats{};
    bool skipped = false;

    bool can_build() const { return !skipped && reason == IslandBuildRejectReason::None; }
};

/// Populate build deepen preflight without mutating the graph (B4.4 deepen follow-up).
IslandBuildDeepenPreflight preflight_island_build_deepen(
    u32 bodyCount,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints);

/// Returns true when island graph build should be skipped (B4.4 deepen follow-up).
bool can_skip_island_build_deepen(
    u32 bodyCount,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints);

/// Build only when deepen preflight passes; clears graph and returns false otherwise (B4.4 deepen follow-up).
bool build_island_graph_with_preflight(ContactIslandGraph& graph,
                                       u32 bodyCount,
                                       const std::vector<narrowphase::ContactManifold>& contacts,
                                       const std::vector<DistanceConstraint>& distanceConstraints);

/// Why island constraint solve would early-out (B4.4 deepen follow-up).
enum class IslandConstraintSolveRejectReason : u8 {
    None = 0,
    EmptyIsland,
    NoInRangeRefs,
    NoMovableBodies,
};

/// Human-readable label for island constraint-solve reject reasons (B4.4 deepen follow-up).
const char* island_constraint_solve_reject_reason_name(IslandConstraintSolveRejectReason reason);

/// Diagnose why island constraint solve would skip (B4.4 deepen follow-up).
IslandConstraintSolveRejectReason island_constraint_solve_reject_reason(
    const ContactIslandGraph::Island& island,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints);

/// Returns true when `island_constraint_solve_reject_reason` matches `expected` (B4.4 deepen follow-up).
bool island_constraint_solve_rejects_for_reason(
    const ContactIslandGraph::Island& island,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints,
    IslandConstraintSolveRejectReason expected);

/// Const preflight for island constraint solve with reject reason (B4.4 deepen follow-up).
struct IslandConstraintSolveDeepenPreflight {
    IslandConstraintSolveRejectReason reason = IslandConstraintSolveRejectReason::None;
    IslandConstraintSolvePreflight solve{};
    bool skipped = false;

    bool can_solve() const { return !skipped && reason == IslandConstraintSolveRejectReason::None; }
};

/// Populate constraint-solve deepen preflight without mutating bodies (B4.4 deepen follow-up).
IslandConstraintSolveDeepenPreflight preflight_island_constraint_solve_deepen(
    const ContactIslandGraph::Island& island,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints);

/// Preflight constraint solve by island index; out-of-range indices are marked skipped (B4.4 deepen follow-up).
IslandConstraintSolvePreflight preflight_island_constraint_solve_by_index(
    const ContactIslandGraph& graph,
    u32 islandIndex,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints);

/// Preflight body participation by island index; out-of-range indices are marked skipped (B4.4 deepen follow-up).
IslandSolveBodiesPreflight preflight_island_solve_bodies_by_index(const ContactIslandGraph& graph,
                                                                  u32 islandIndex,
                                                                  const RigidBodySoA& bodies);

/// Early-out guard for constraint solve by island index (B4.4 deepen follow-up).
bool should_skip_island_constraint_solve_by_index(const ContactIslandGraph& graph,
                                                  u32 islandIndex,
                                                  const RigidBodySoA& bodies,
                                                  const std::vector<narrowphase::ContactManifold>& contacts,
                                                  const std::vector<DistanceConstraint>& distanceConstraints);

/// Returns true when constraint solve should be skipped (B4.4 deepen follow-up).
bool can_skip_island_constraint_solve_deepen(
    const ContactIslandGraph::Island& island,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints);

/// Resolve constraints only when deepen preflight passes (B4.4 deepen follow-up).
bool solve_island_job_with_preflight(RigidBodySoA& bodies,
                                     const ContactIslandGraph::Island& island,
                                     SolverWorkBuffers& workBuffers,
                                     const std::vector<DistanceConstraint>& distanceConstraints,
                                     f32 dt,
                                     f32 contactCompliance,
                                     const std::function<f32(const RigidBodySoA&, u32)>& invMassFn);

/// Why island sleep-solve skip would not apply (B4.4 deepen follow-up).
enum class IslandSleepRejectReason : u8 {
    None = 0,
    EmptyIsland,
    OutOfRangeIndex,
    NotAllSleeping,
};

/// Human-readable label for island sleep reject reasons (B4.4 deepen follow-up).
const char* island_sleep_reject_reason_name(IslandSleepRejectReason reason);

/// Diagnose why an island cannot skip solve due to sleep (B4.4 deepen follow-up).
IslandSleepRejectReason island_sleep_reject_reason(const ContactIslandGraph::Island& island,
                                                 const RigidBodySoA& bodies);

/// Why island wake would early-out (B4.4 deepen follow-up).
enum class IslandWakeRejectReason : u8 {
    None = 0,
    EmptyIsland,
    OutOfRangeIndex,
    NoMixedSleepState,
    NoActiveDynamic,
};

/// Human-readable label for island wake reject reasons (B4.4 deepen follow-up).
const char* island_wake_reject_reason_name(IslandWakeRejectReason reason);

/// Diagnose why island wake would skip (B4.4 deepen follow-up).
IslandWakeRejectReason island_wake_reject_reason(const ContactIslandGraph::Island& island,
                                                 const RigidBodySoA& bodies);

/// Per-island wake outcome (skip vs wake) for parallel batch stubs (B4.4 deepen follow-up).
struct IslandWakeResult {
    bool woke = false;
    bool skipped = false;
    u32 islandIndex = ContactIslandGraph::invalidIsland;
};

/// Guarded wake with explicit skip/wake outcome (B4.4 deepen follow-up).
IslandWakeResult wake_island_sleepers_result(RigidBodySoA& bodies,
                                             const ContactIslandGraph& graph,
                                             u32 islandIndex);

/// Combined job + constraint-solve + wake preflight for guarded dispatch (B4.4 deepen follow-up).
struct IslandFullDispatchPreflight {
    IslandSolveJobPreflight job{};
    IslandConstraintSolveDeepenPreflight constraintSolve{};
    IslandWakePreflight wake{};
    bool skipped = false;

    bool can_dispatch() const {
        return !skipped && job.can_dispatch() && constraintSolve.can_solve();
    }
};

/// Preflight guarded island dispatch including wake hints (B4.4 deepen follow-up).
IslandFullDispatchPreflight preflight_dispatch_solve_island(
    const ContactIslandGraph& graph,
    u32 islandIndex,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints,
    f32 dt);

/// Guarded dispatch: wake mixed islands, then solve when deepen preflight passes (B4.4 deepen follow-up).
bool dispatch_solve_island_with_preflight(RigidBodySoA& bodies,
                                          const ContactIslandGraph& graph,
                                          u32 islandIndex,
                                          SolverWorkBuffers& workBuffers,
                                          const std::vector<DistanceConstraint>& distanceConstraints,
                                          f32 dt,
                                          f32 contactCompliance,
                                          const std::function<f32(const RigidBodySoA&, u32)>& invMassFn);

/// Guarded dispatch with explicit skip/solve outcome (B4.4 deepen follow-up).
IslandDispatchResult dispatch_solve_island_with_preflight_result(
    RigidBodySoA& bodies,
    const ContactIslandGraph& graph,
    u32 islandIndex,
    SolverWorkBuffers& workBuffers,
    const std::vector<DistanceConstraint>& distanceConstraints,
    f32 dt,
    f32 contactCompliance,
    const std::function<f32(const RigidBodySoA&, u32)>& invMassFn);

/// Batch guarded dispatch with wake + constraint-solve preflights (B4.4 deepen follow-up).
IslandBatchDispatchResult dispatch_all_islands_with_preflight_result(
    RigidBodySoA& bodies,
    const ContactIslandGraph& graph,
    SolverWorkBuffers& workBuffers,
    const std::vector<DistanceConstraint>& distanceConstraints,
    f32 dt,
    f32 contactCompliance,
    const std::function<f32(const RigidBodySoA&, u32)>& invMassFn);

/// Combined job, sleep, and constraint-solve preflight for one island dispatch.
struct IslandSleepAwareDispatchPreflight {
    IslandSolveJobPreflight job{};
    IslandSleepPreflight sleep{};
    IslandConstraintSolvePreflight constraint{};
    bool skipped = false;

    bool can_dispatch() const {
        return !skipped && job.can_dispatch() && !sleep.can_skip_solve() && constraint.can_solve();
    }
};

/// Graph-level dispatch + sleep preflight for selective nonsleeping batch guards.
struct IslandSleepAwareGraphPreflight {
    IslandDispatchPreflight dispatch{};
    IslandSleepGraphPreflight sleep{};
    bool skipped = false;

    bool can_dispatch() const { return !skipped && dispatch.can_dispatch() && sleep.has_solveable_islands(); }
};

/// Batch sleep-aware dispatch summary (skip vs solve) for parallel iteration stubs.
struct IslandBatchSleepAwareDispatchResult {
    u32 solvedCount = 0;
    u32 skippedSleepCount = 0;
    u32 skippedConstraintCount = 0;
    u32 skippedDispatchCount = 0;
    u32 dispatchableCount = 0;
    bool skipped = false;

    bool any_solved() const { return solvedCount > 0u; }
};

/// Preflight body participation by island index; out-of-range indices are marked skipped.
IslandSolveBodiesPreflight preflight_island_solve_bodies_by_index(const ContactIslandGraph& graph,
                                                                  u32 islandIndex,
                                                                  const RigidBodySoA& bodies);

/// Combined constraint-ref + body participation preflight by island index.
IslandConstraintSolvePreflight preflight_island_constraint_solve_by_index(
    const ContactIslandGraph& graph,
    u32 islandIndex,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints);

/// Preflight one island for sleep-aware dispatch (job + sleep + constraint guards).
IslandSleepAwareDispatchPreflight preflight_island_sleep_aware_dispatch(
    const ContactIslandGraph::Island& island,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints,
    f32 dt);

/// Preflight sleep-aware dispatch by island index; out-of-range indices are marked skipped.
IslandSleepAwareDispatchPreflight preflight_island_sleep_aware_dispatch_by_index(
    const ContactIslandGraph& graph,
    u32 islandIndex,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints,
    f32 dt);

/// Preflight sleep-aware dispatch from a pre-extracted job.
IslandSleepAwareDispatchPreflight preflight_island_sleep_aware_dispatch_job(
    const IslandSolveJob& job,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints,
    f32 dt);

/// Graph-level sleep-aware dispatch preflight; sets `skipped` when nothing is dispatchable.
IslandSleepAwareGraphPreflight preflight_island_sleep_aware_graph(const ContactIslandGraph& graph,
                                                                  const RigidBodySoA& bodies,
                                                                  f32 dt);

/// Early-out guard when sleep, constraint, or job preflight blocks island dispatch.
bool should_skip_island_sleep_aware_dispatch(const ContactIslandGraph::Island& island,
                                             const RigidBodySoA& bodies,
                                             const std::vector<narrowphase::ContactManifold>& contacts,
                                             const std::vector<DistanceConstraint>& distanceConstraints,
                                             f32 dt);

/// Collect island indices that pass constraint-solve preflight (parallel solve prep stub).
std::vector<u32> collect_solveable_island_indices(
    const ContactIslandGraph& graph,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints);

/// Guarded island solve; returns false when constraint-solve preflight blocks work.
bool solve_island_job_guarded(RigidBodySoA& bodies,
                              const ContactIslandGraph::Island& island,
                              SolverWorkBuffers& workBuffers,
                              const std::vector<DistanceConstraint>& distanceConstraints,
                              f32 dt,
                              f32 contactCompliance,
                              const std::function<f32(const RigidBodySoA&, u32)>& invMassFn);

/// Guarded dispatch; skips all-sleeping islands without changing valid awake paths.
bool dispatch_solve_island_sleep_guarded(RigidBodySoA& bodies,
                                         const ContactIslandGraph& graph,
                                         u32 islandIndex,
                                         SolverWorkBuffers& workBuffers,
                                         const std::vector<DistanceConstraint>& distanceConstraints,
                                         f32 dt,
                                         f32 contactCompliance,
                                         const std::function<f32(const RigidBodySoA&, u32)>& invMassFn);

/// Guarded dispatch; wakes mixed-island sleepers before solve when preflight requests it.
bool dispatch_solve_island_with_wake_guarded(RigidBodySoA& bodies,
                                             const ContactIslandGraph& graph,
                                             u32 islandIndex,
                                             SolverWorkBuffers& workBuffers,
                                             const std::vector<DistanceConstraint>& distanceConstraints,
                                             f32 dt,
                                             f32 contactCompliance,
                                             const std::function<f32(const RigidBodySoA&, u32)>& invMassFn);

/// Guarded dispatch combining wake, sleep, and constraint-solve preflights.
bool dispatch_solve_island_sleep_aware_guarded(RigidBodySoA& bodies,
                                               const ContactIslandGraph& graph,
                                               u32 islandIndex,
                                               SolverWorkBuffers& workBuffers,
                                               const std::vector<DistanceConstraint>& distanceConstraints,
                                               f32 dt,
                                               f32 contactCompliance,
                                               const std::function<f32(const RigidBodySoA&, u32)>& invMassFn);

/// Guarded sleep-aware dispatch with explicit skip/solve outcome.
IslandDispatchResult dispatch_solve_island_sleep_aware_result(RigidBodySoA& bodies,
                                                              const ContactIslandGraph& graph,
                                                              u32 islandIndex,
                                                              SolverWorkBuffers& workBuffers,
                                                              const std::vector<DistanceConstraint>& distanceConstraints,
                                                              f32 dt,
                                                              f32 contactCompliance,
                                                              const std::function<f32(const RigidBodySoA&, u32)>& invMassFn);

/// Batch guarded dispatch over nonsleeping islands; returns count actually solved.
u32 dispatch_nonsleeping_islands_guarded(RigidBodySoA& bodies,
                                         const ContactIslandGraph& graph,
                                         SolverWorkBuffers& workBuffers,
                                         const std::vector<DistanceConstraint>& distanceConstraints,
                                         f32 dt,
                                         f32 contactCompliance,
                                         const std::function<f32(const RigidBodySoA&, u32)>& invMassFn);

/// Batch guarded nonsleeping dispatch with explicit skip/solve counts.
IslandBatchSleepAwareDispatchResult dispatch_nonsleeping_islands_result(
    RigidBodySoA& bodies,
    const ContactIslandGraph& graph,
    SolverWorkBuffers& workBuffers,
    const std::vector<DistanceConstraint>& distanceConstraints,
    f32 dt,
    f32 contactCompliance,
    const std::function<f32(const RigidBodySoA&, u32)>& invMassFn);

/// Guarded wake with explicit skip/wake outcome and bodies-woken count.
IslandWakeResult wake_island_sleepers_result(RigidBodySoA& bodies,
                                             const ContactIslandGraph& graph,
                                             u32 islandIndex);

/// Batch guarded wake with explicit skip/wake counts.
IslandBatchWakeResult wake_all_island_sleepers_result(RigidBodySoA& bodies,
                                                      const ContactIslandGraph& graph);

/// Preflight island dispatch including sleep graph guards.
IslandSleepDispatchPreflight preflight_island_sleep_dispatch(const ContactIslandGraph& graph,
                                                             const RigidBodySoA& bodies,
                                                             f32 dt);

/// Early-out guard when dispatch or sleep graph preflight blocks solve.
bool should_skip_island_sleep_dispatch(const ContactIslandGraph& graph,
                                       const RigidBodySoA& bodies,
                                       f32 dt);

/// Wake wakeable islands then dispatch solveable islands with body guards.
IslandBatchSleepDispatchResult dispatch_all_islands_wake_and_solve_result(
    RigidBodySoA& bodies,
    const ContactIslandGraph& graph,
    SolverWorkBuffers& workBuffers,
    const std::vector<DistanceConstraint>& distanceConstraints,
    f32 dt,
    f32 contactCompliance,
    const std::function<f32(const RigidBodySoA&, u32)>& invMassFn);

/// Guarded wake with explicit skip/woke outcome.
IslandWakeResult wake_island_sleepers_result(RigidBodySoA& bodies,
                                             const ContactIslandGraph& graph,
                                             u32 islandIndex);

/// Batch guarded wake with explicit skip/woke counts.
IslandBatchWakeResult wake_all_island_sleepers_result(RigidBodySoA& bodies,
                                                      const ContactIslandGraph& graph);

/// Graph-level wake-then-solve dispatch preflight.
IslandDispatchSolveablePreflight preflight_island_dispatch_solveable(const ContactIslandGraph& graph,
                                                                     const RigidBodySoA& bodies,
                                                                     const std::vector<narrowphase::ContactManifold>& contacts,
                                                                     const std::vector<DistanceConstraint>& distanceConstraints,
                                                                     f32 dt);

/// Early-out guard for wake-then-solve batch dispatch.
bool should_skip_island_dispatch_solveable(const ContactIslandGraph& graph,
                                           const RigidBodySoA& bodies,
                                           const std::vector<narrowphase::ContactManifold>& contacts,
                                           const std::vector<DistanceConstraint>& distanceConstraints,
                                           f32 dt);

/// Wake mixed islands then dispatch only solveable islands; returns solved count.
u32 dispatch_solveable_islands_guarded(RigidBodySoA& bodies,
                                       const ContactIslandGraph& graph,
                                       SolverWorkBuffers& workBuffers,
                                       const std::vector<DistanceConstraint>& distanceConstraints,
                                       f32 dt,
                                       f32 contactCompliance,
                                       const std::function<f32(const RigidBodySoA&, u32)>& invMassFn);

/// Wake-then-solve batch dispatch with explicit skip/woke/solved counts.
IslandBatchDispatchSolveableResult dispatch_solveable_islands_result(
    RigidBodySoA& bodies,
    const ContactIslandGraph& graph,
    SolverWorkBuffers& workBuffers,
    const std::vector<DistanceConstraint>& distanceConstraints,
    f32 dt,
    f32 contactCompliance,
    const std::function<f32(const RigidBodySoA&, u32)>& invMassFn);

} // namespace fuse::physics
