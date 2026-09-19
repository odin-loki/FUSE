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
/// Why island graph build would reject (B4.4 deepen follow-up pass).
enum class IslandBuildRejectReason : u8 {
    None = 0,
    EmptyInput,
    OutOfRangeRefs,
};

/// Why graph-level island dispatch would reject (B4.4 deepen follow-up pass).
enum class IslandDispatchRejectReason : u8 {
    EmptyGraph,
    InvalidDt,
    NonFiniteDt,

/// Why per-island solve job dispatch would reject (B4.4 deepen follow-up pass).
enum class IslandSolveJobRejectReason : u8 {
    OutOfRangeIndex,
    EmptyIsland,

/// Why per-island constraint solve would reject (B4.4 deepen follow-up pass).
enum class IslandConstraintSolveRejectReason : u8 {
    NoInRangeRefs,
    NoMovableBodies,

/// Why per-island sleep solve would skip (B4.4 deepen follow-up pass).
enum class IslandSleepRejectReason : u8 {
    AllSleeping,

/// Why per-island wake would skip (B4.4 deepen follow-up pass).
enum class IslandWakeRejectReason : u8 {
    NoWakeTarget,
    NoDispatchableIslands,

/// Why per-job island dispatch would reject (B4.4 deepen follow-up pass).
enum class IslandJobDispatchRejectReason : u8 {

    StaleRefs,

/// Why per-island sleep preflight would skip solve (B4.4 deepen follow-up pass).

/// Why per-island wake preflight would skip activation (B4.4 deepen follow-up pass).
    NoMixedSleep,
/// Reject reason for island graph build inputs (B4.4 deepen, B4.2 parity).
    OutOfRangeContactBody,
    OutOfRangeDistanceBody,

const char* islandBuildRejectReasonName(IslandBuildRejectReason reason);

IslandBuildRejectReason islandBuildRejectReason(
    u32 bodyCount,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints);

bool islandBuildRejectsForReason(u32 bodyCount,
                                 const std::vector<DistanceConstraint>& distanceConstraints,
                                 IslandBuildRejectReason expected);

/// Reject reason for island constraint-solve preflight (B4.4 deepen, B4.2 parity).
    StaleContactRefs,
    StaleDistanceRefs,

const char* islandConstraintSolveRejectReasonName(IslandConstraintSolveRejectReason reason);

IslandConstraintSolveRejectReason islandConstraintSolveRejectReason(
    const ContactIslandGraph::Island& island,
    const RigidBodySoA& bodies,

bool islandConstraintSolveRejectsForReason(const ContactIslandGraph::Island& island,
                                           IslandConstraintSolveRejectReason expected);

/// Reject reason for island solve dispatch (B4.4 deepen, B4.2 parity).
    EmptyJob,
    OutOfRangeIsland,
    NoConstraints,
    StaleConstraintRefs,

const char* islandDispatchRejectReasonName(IslandDispatchRejectReason reason);

/// Reject reason for island warm-start preflight (B4.4 deepen, B4.2 parity).
enum class IslandWarmStartRejectReason : u8 {
    NoPriorData,
    NoImpulses,

const char* islandWarmStartRejectReasonName(IslandWarmStartRejectReason reason);

IslandWarmStartRejectReason islandWarmStartRejectReason(const ContactIslandGraph::Island& island,
                                                        const std::vector<f32>& priorDistanceLambdas,
                                                        const std::vector<f32>& priorContactLambdas = {});

IslandWarmStartRejectReason islandWarmStartRejectReason(
    f32 dt);

bool islandWarmStartRejectsForReason(const ContactIslandGraph::Island& island,
                                     const std::vector<f32>& priorContactLambdas,
                                     IslandWarmStartRejectReason expected);
struct IslandSolveJob;

/// Why island solve dispatch would early-out (B4.4 deepen follow-up pass).

/// Human-readable label for island dispatch reject reasons (logging / tests).
const char* island_dispatch_reject_reason_name(IslandDispatchRejectReason reason);

/// Returns the first reject reason for graph-level island dispatch, or `None` when dispatch may proceed.
IslandDispatchRejectReason island_dispatch_reject_reason(const ContactIslandGraph& graph, f32 dt);

/// Returns true when `island_dispatch_reject_reason` matches `expected` (B4.4 deepen follow-up pass).
bool island_dispatch_rejects_for_reason(const ContactIslandGraph& graph,
                                        f32 dt,
                                        IslandDispatchRejectReason expected);

/// Why one island solve job would early-out (B4.4 deepen follow-up pass).

/// Human-readable label for island solve job reject reasons (logging / tests).
const char* island_solve_job_reject_reason_name(IslandSolveJobRejectReason reason);

/// Returns the first reject reason for one extracted job, or `None` when dispatch may proceed.
IslandSolveJobRejectReason island_solve_job_reject_reason(const IslandSolveJob& job, f32 dt);

/// Returns true when `island_solve_job_reject_reason` matches `expected` (B4.4 deepen follow-up pass).
bool island_solve_job_rejects_for_reason(const IslandSolveJob& job,
                                         IslandSolveJobRejectReason expected);

/// Why island constraint refs would block solve (B4.4 deepen follow-up pass).
enum class IslandConstraintRefsRejectReason : u8 {

/// Human-readable label for constraint-ref reject reasons (logging / tests).
const char* island_constraint_refs_reject_reason_name(IslandConstraintRefsRejectReason reason);

/// Returns the first reject reason for island constraint refs, or `None` when refs are solvable.
IslandConstraintRefsRejectReason island_constraint_refs_reject_reason(

/// Returns true when `island_constraint_refs_reject_reason` matches `expected` (B4.4 deepen follow-up pass).
bool island_constraint_refs_rejects_for_reason(
    IslandConstraintRefsRejectReason expected);

/// Why island constraint solve would early-out (B4.4 deepen follow-up pass).

/// Human-readable label for constraint-solve reject reasons (logging / tests).
const char* island_constraint_solve_reject_reason_name(IslandConstraintSolveRejectReason reason);

/// Returns the first reject reason for one island constraint solve pass, or `None` when solvable.
IslandConstraintSolveRejectReason island_constraint_solve_reject_reason(

/// Returns true when `island_constraint_solve_reject_reason` matches `expected` (B4.4 deepen follow-up pass).
bool island_constraint_solve_rejects_for_reason(
    NullIslandJob,
    ZeroConstraints,

/// Why one island constraint solve would early-out (B4.4 deepen follow-up pass).
enum class IslandSolveRejectReason : u8 {
    None = 0,
    EmptyIsland,
    OutOfRangeIsland,
    InvalidDt,
    OutOfRangeRefs,
    NoMovableBodies,
};

/// Human-readable label for island solve reject reasons (logging / tests).
const char* island_solve_reject_reason_name(IslandSolveRejectReason reason);

/// Why island sleep preflight would early-out (B4.4 deepen follow-up pass).
enum class IslandSleepRejectReason : u8 {
    AllSleeping,

/// Human-readable label for island sleep reject reasons (logging / tests).
const char* island_sleep_reject_reason_name(IslandSleepRejectReason reason);

/// Returns the first reject reason for island sleep preflight, or `None` when solve may proceed.
IslandSleepRejectReason island_sleep_reject_reason(const ContactIslandGraph::Island& island,
                                                   const RigidBodySoA& bodies);

/// Returns the first reject reason by island index, or `None` when solve may proceed.
IslandSleepRejectReason island_sleep_reject_reason_by_index(const ContactIslandGraph& graph,
                                                            u32 islandIndex,

/// Returns true when `island_sleep_reject_reason` matches `expected` (B4.4 deepen follow-up pass).
bool island_sleep_rejects_for_reason(const ContactIslandGraph::Island& island,
                                     IslandSleepRejectReason expected);

/// Why graph-level island sleep batching would skip solve (B4.4 deepen follow-up pass).
enum class IslandSleepGraphRejectReason : u8 {
    AllIslandsSleeping,

/// Human-readable label for graph sleep reject reasons (logging / tests).
const char* island_sleep_graph_reject_reason_name(IslandSleepGraphRejectReason reason);

/// Returns the first reject reason for graph sleep preflight, or `None` when solve may proceed.
IslandSleepGraphRejectReason island_sleep_graph_reject_reason(const ContactIslandGraph& graph,

/// Returns true when `island_sleep_graph_reject_reason` matches `expected` (B4.4 deepen follow-up pass).
bool island_sleep_graph_rejects_for_reason(const ContactIslandGraph& graph,
                                           IslandSleepGraphRejectReason expected);

/// Why island wake preflight would early-out (B4.4 deepen follow-up pass).
    NoInRangeRefs,

/// Why island sleep preflight would skip (B4.4 deepen follow-up pass).
    None = 0,
    NoSolveableIslands,

/// Why island wake preflight would skip (B4.4 deepen follow-up pass).
enum class IslandWakeRejectReason : u8 {
    None = 0,
    EmptyIsland,
    OutOfRangeIsland,
    NoWakeTarget,
};

/// Human-readable label for island wake reject reasons (logging / tests).
const char* island_wake_reject_reason_name(IslandWakeRejectReason reason);

/// Returns the first reject reason for island wake preflight, or `None` when wake may proceed.
IslandWakeRejectReason island_wake_reject_reason(const ContactIslandGraph::Island& island,

/// Returns the first reject reason by island index, or `None` when wake may proceed.
IslandWakeRejectReason island_wake_reject_reason_by_index(const ContactIslandGraph& graph,

/// Returns true when `island_wake_reject_reason` matches `expected` (B4.4 deepen follow-up pass).
bool island_wake_rejects_for_reason(const ContactIslandGraph::Island& island,
                                    IslandWakeRejectReason expected);

/// Why graph-level island wake batching would skip activation (B4.4 deepen follow-up pass).
enum class IslandWakeGraphRejectReason : u8 {
    NoWakeableIslands,

/// Human-readable label for graph wake reject reasons (logging / tests).
const char* island_wake_graph_reject_reason_name(IslandWakeGraphRejectReason reason);

/// Returns the first reject reason for graph wake preflight, or `None` when wake may proceed.
IslandWakeGraphRejectReason island_wake_graph_reject_reason(const ContactIslandGraph& graph,

/// Returns true when `island_wake_graph_reject_reason` matches `expected` (B4.4 deepen follow-up pass).
bool island_wake_graph_rejects_for_reason(const ContactIslandGraph& graph,

/// Why island pipeline dispatch would early-out (B4.4 deepen follow-up pass).

/// Why per-job island dispatch would early-out (B4.4 deepen follow-up pass).

    NoInRangeConstraints,

/// Why island sleep preflight would skip solve (B4.4 deepen follow-up pass).

/// Why graph-level island sleep batching would early-out (B4.4 deepen follow-up pass).
    NoSolveableIslands,

/// Why island wake preflight would skip activation (B4.4 deepen follow-up pass).
    NoMixedState,

/// Why graph-level island wake batching would early-out (B4.4 deepen follow-up pass).



                                          IslandWakeGraphRejectReason expected);
/// Why island solve dispatch would be rejected (B4.4 deepen follow-up).
    NothingDispatchable,

/// Why one island constraint solve would be rejected (B4.4 deepen follow-up).
enum class IslandSolveRejectReason : u8 {
    OutOfRangeIslandIndex,

/// Why an island sleep skip-solve preflight would not apply (B4.4 deepen follow-up).
    NotAllSleeping,

/// Why an island wake preflight would not activate sleepers (B4.4 deepen follow-up).
    NoMixedSleepState,

/// Why graph-level sleep batching would skip all islands (B4.4 deepen follow-up).

/// Why graph-level wake batching would skip all islands (B4.4 deepen follow-up).

const char* island_solve_reject_reason_name(IslandSolveRejectReason reason);

IslandSolveRejectReason island_constraint_solve_reject_reason(


/// Human-readable labels for island reject reasons (logging / tests).
const char* island_dispatch_reject_reason_name(IslandDispatchRejectReason reason);
const char* island_sleep_reject_reason_name(IslandSleepRejectReason reason);

/// Lightweight view for parallel island dispatch (B4.4 deepen).
struct IslandSolveJob {
    u32 islandIndex = ContactIslandGraph::invalidIsland;
    u32 constraintCount = 0;
    bool empty = true;
    const ContactIslandGraph::Island* island = nullptr;
};

/// Diagnostic reason island solve dispatch would skip (B4.4 deepen follow-up).
enum class IslandDispatchRejectReason : u8 {
    None = 0,
    NoDispatchableIslands,
    InvalidDt,
    NonFiniteDt,
};

/// Diagnostic reason one island solve job would skip dispatch (B4.4 deepen follow-up).
enum class IslandSolveJobRejectReason : u8 {
    EmptyJob,

/// Diagnostic reason one island constraint solve would skip (B4.4 deepen follow-up).
enum class IslandConstraintSolveRejectReason : u8 {
    EmptyIsland,
    StaleConstraintRefs,
    NoMovableBodies,

/// Diagnostic reason one island sleep preflight would skip solve (B4.4 deepen follow-up).
enum class IslandSleepRejectReason : u8 {
    OutOfRangeIsland,
    AllSleeping,

/// Diagnostic reason graph-level island sleep batch would skip (B4.4 deepen follow-up).
enum class IslandSleepGraphRejectReason : u8 {
    NoSolveableIslands,

/// Diagnostic reason one island wake preflight would skip activation (B4.4 deepen follow-up).
enum class IslandWakeRejectReason : u8 {
    NoMixedSleepState,

/// Diagnostic reason graph-level island wake batch would skip (B4.4 deepen follow-up).
enum class IslandWakeGraphRejectReason : u8 {
    NoWakeableIslands,

/// Human-readable labels for island pipeline reject reasons (logging / tests).
const char* island_dispatch_reject_reason_name(IslandDispatchRejectReason reason);
const char* island_solve_job_reject_reason_name(IslandSolveJobRejectReason reason);
const char* island_constraint_solve_reject_reason_name(IslandConstraintSolveRejectReason reason);
const char* island_sleep_reject_reason_name(IslandSleepRejectReason reason);
const char* island_sleep_graph_reject_reason_name(IslandSleepGraphRejectReason reason);
const char* island_wake_reject_reason_name(IslandWakeRejectReason reason);
const char* island_wake_graph_reject_reason_name(IslandWakeGraphRejectReason reason);
IslandSolveRejectReason island_solve_job_reject_reason(const IslandSolveJob& job, f32 dt);

/// Aggregate counts for parallel dispatch sizing and empty-island early-out stubs.
struct IslandSolveStats {
    u32 totalIslands = 0;
    u32 constrainedCount = 0;
    u32 emptyCount = 0;
    u32 dispatchableCount = 0;
};

/// Why island solve dispatch would skip (B4.4 deepen pass).
/// Why island solve dispatch would early-out (B4.4 deepen follow-up).
/// Why island solve dispatch would early-out (B4.4 deepen follow-up pass).
enum class IslandSolveRejectReason : u8 {
    None = 0,
    NoDispatchableIslands,
};

/// Human-readable label for island solve reject reasons (logging / tests).
const char* islandSolveRejectReasonName(IslandSolveRejectReason reason);

/// Diagnose why graph-level island solve would skip; vacuously succeeds when dispatch may proceed.
IslandSolveRejectReason islandSolveRejectReason(const ContactIslandGraph& graph);

/// Returns true when `islandSolveRejectReason` matches `expected` (B4.4 deepen pass).
bool islandSolveRejectsForReason(const ContactIslandGraph& graph, IslandSolveRejectReason expected);
/// Why island solve dispatch would reject (B4.4 deepen pass).
enum class IslandDispatchRejectReason : u8 {
    InvalidDt,

/// Human-readable label for island dispatch reject reasons (logging / tests).
const char* islandDispatchRejectReasonName(IslandDispatchRejectReason reason);

/// Why per-job island dispatch would reject (B4.4 deepen pass).
enum class IslandSolveJobRejectReason : u8 {
    EmptyJob,
    ZeroConstraints,

/// Human-readable label for per-job island dispatch reject reasons (logging / tests).
const char* islandSolveJobRejectReasonName(IslandSolveJobRejectReason reason);
/// Why island batch dispatch would early-out (B4.4 deepen follow-up).

/// Why one island solve job would early-out (B4.4 deepen follow-up).

/// Why island constraint solve would early-out (B4.4 deepen follow-up).
enum class IslandConstraintSolveRejectReason : u8 {
    EmptyIsland,
    NoInRangeConstraints,
    NoMovableBodies,

/// Why per-island sleep preflight would skip solve (B4.4 deepen follow-up).
enum class IslandSleepRejectReason : u8 {
    OutOfRangeIndex,
    AllSleeping,

/// Why graph-level sleep preflight would skip solve (B4.4 deepen follow-up).
enum class IslandSleepGraphRejectReason : u8 {
    NoSolveableIslands,

/// Why per-island wake preflight would skip activation (B4.4 deepen follow-up).
enum class IslandWakeRejectReason : u8 {
    NoWakeTarget,

/// Why graph-level wake preflight would skip activation (B4.4 deepen follow-up).
enum class IslandWakeGraphRejectReason : u8 {
    NoWakeableIslands,

/// Human-readable labels for island pipeline reject reasons (logging / tests).
const char* islandConstraintSolveRejectReasonName(IslandConstraintSolveRejectReason reason);
const char* islandSleepRejectReasonName(IslandSleepRejectReason reason);
const char* islandSleepGraphRejectReasonName(IslandSleepGraphRejectReason reason);
const char* islandWakeRejectReasonName(IslandWakeRejectReason reason);
const char* islandWakeGraphRejectReasonName(IslandWakeGraphRejectReason reason);
/// Why island solve dispatch would reject the graph batch (B4.4 deepen follow-up).

const char* island_dispatch_reject_reason_name(IslandDispatchRejectReason reason);

/// Why per-island constraint solve would reject dispatch (B4.4 deepen follow-up).
    NoConstraints,
    StaleConstraintRefs,

const char* island_solve_reject_reason_name(IslandSolveRejectReason reason);

/// Why island sleep preflight would not skip solve (B4.4 deepen follow-up).
    NotAllSleeping,

/// Human-readable label for island sleep reject reasons (logging / tests).
const char* island_sleep_reject_reason_name(IslandSleepRejectReason reason);

/// Why island wake preflight would not activate sleepers (B4.4 deepen follow-up).
    UniformSleepState,

/// Human-readable label for island wake reject reasons (logging / tests).
const char* island_wake_reject_reason_name(IslandWakeRejectReason reason);

/// Human-readable label for island solve reject reasons (B4.4 deepen follow-up pass).

/// Preflight diagnostics for island solve dispatch (B4.4 deepen).
struct IslandSolvePreflight {
    IslandSolveRejectReason reason = IslandSolveRejectReason::None;
    IslandDispatchRejectReason reason = IslandDispatchRejectReason::None;
    IslandSolveStats stats{};
    IslandDispatchRejectReason reason = IslandDispatchRejectReason::None;
    IslandSolveRejectReason reason = IslandSolveRejectReason::None;
    bool skipped = false;
    bool noDispatchableIslands = false;

    bool can_dispatch() const { return !skipped && reason == IslandSolveRejectReason::None; }
    bool can_dispatch() const { return reason == IslandDispatchRejectReason::None && !skipped; }
    bool can_dispatch() const { return reason == IslandDispatchRejectReason::None && !skipped && stats.dispatchableCount > 0u; }
    bool can_dispatch() const { return !skipped && reason == IslandSolveRejectReason::None && stats.dispatchableCount > 0u; }
};

/// Why island dispatch would early-out (B4.4 deepen pass).
enum class IslandDispatchRejectReason : u8 {
    None = 0,
    NoDispatchableIslands,
    InvalidDt,
};

/// Human-readable label for island dispatch reject reasons (logging / tests).
const char* islandDispatchRejectReasonName(IslandDispatchRejectReason reason);

/// Diagnose why island dispatch would skip; vacuously succeeds when work is dispatchable.
IslandDispatchRejectReason islandDispatchRejectReason(const ContactIslandGraph& graph, f32 dt);

/// Returns true when `islandDispatchRejectReason` matches `expected` (B4.4 deepen pass).
bool islandDispatchRejectsForReason(const ContactIslandGraph& graph,
                                    f32 dt,
                                    IslandDispatchRejectReason expected);
/// Diagnose why island solve dispatch would skip.
IslandSolveRejectReason islandSolveRejectReason(const IslandSolvePreflight& preflight);

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
    IslandDispatchRejectReason reason = IslandDispatchRejectReason::None;
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

    bool can_wake() const { return !skipped && shouldWake; }

/// Body-partition diagnostics across all islands (B4.4 deepen).
struct IslandBodyPartitionPreflight {
    u32 totalIslands = 0;
    u32 totalBodySlots = 0;
    u32 duplicateBodyCount = 0;
    bool partitionValid = true;

    bool can_dispatch() const { return !skipped && partitionValid; }

/// Constraint-iteration preflight for solver params (B4.4 deepen).
struct ConstraintIterationPreflight {
    u32 iterations = 0;
    f32 residualTolerance = 0.f;
    bool zeroIterations = false;

    bool can_iterate() const { return !skipped && !zeroIterations; }

/// Why island batch dispatch would skip (B4.4 deepen pass).
/// Why island solve dispatch would early-out (B4.4 deepen follow-up pass).
/// Why island batch dispatch would early-out (B4.4 deepen follow-up pass).
enum class IslandDispatchRejectReason : u8 {
    None = 0,
    NoDispatchableIslands,
    InvalidDt,
};

/// Human-readable label for island dispatch reject reasons (logging / tests).
const char* islandDispatchRejectReasonName(IslandDispatchRejectReason reason);

/// Diagnose why batch island dispatch would skip; vacuously succeeds when dispatch may proceed.
IslandDispatchRejectReason islandDispatchRejectReason(const ContactIslandGraph& graph, f32 dt);

/// Returns true when `islandDispatchRejectReason` matches `expected` (B4.4 deepen pass).
bool islandDispatchRejectsForReason(const ContactIslandGraph& graph,
                                    f32 dt,
                                    IslandDispatchRejectReason expected);
    NonFiniteDt,

const char* island_dispatch_reject_reason_name(IslandDispatchRejectReason reason);

/// Diagnose why dispatch would skip; vacuously succeeds when dispatch may proceed.
IslandDispatchRejectReason island_dispatch_reject_reason(const ContactIslandGraph& graph, f32 dt);

/// Returns true when `island_dispatch_reject_reason` matches `expected`.
bool island_dispatch_rejects_for_reason(const ContactIslandGraph& graph,

/// Human-readable label for island dispatch reject reasons (B4.4 deepen follow-up pass).

/// Combined dispatch preflight (graph + timestep guard).
struct IslandDispatchPreflight {
    IslandDispatchRejectReason reason = IslandDispatchRejectReason::None;
    IslandSolvePreflight solve{};
    IslandDispatchRejectReason reason = IslandDispatchRejectReason::None;
    bool invalidDt = false;

    bool can_dispatch() const { return !skipped && !invalidDt && solve.can_dispatch(); }

/// Preflight island dispatch from pre-extracted jobs (parallel batch guard).
struct IslandDispatchJobPreflight {
    IslandSolveStats stats{};

    bool can_dispatch() const { return !skipped && !invalidDt && stats.dispatchableCount > 0u; }
    bool nonFiniteDt = false;
    bool skipped = false;

    bool can_dispatch() const { return !skipped && !invalidDt && !nonFiniteDt && solve.can_dispatch(); }
    bool can_dispatch() const { return reason == IslandDispatchRejectReason::None && !skipped; }
    bool can_dispatch() const { return reason == IslandDispatchRejectReason::None; }
    bool can_dispatch() const { return reason == IslandDispatchRejectReason::None && !skipped && !invalidDt && solve.can_dispatch(); }
    bool can_dispatch() const {
        return !skipped && reason == IslandDispatchRejectReason::None && !invalidDt && solve.can_dispatch();
    }
};

/// Post-build island graph consistency (out-of-range body/constraint ref guards).
struct IslandBuiltGraphStats {
    u32 totalIslands = 0;
    u32 constrainedCount = 0;
    u32 emptyCount = 0;
    u32 outOfRangeBodyIndexCount = 0;
    u32 outOfRangeContactRefCount = 0;
    u32 outOfRangeDistanceRefCount = 0;

/// Preflight diagnostics for a built island graph (B4.4 deepen).
struct IslandBuiltGraphPreflight {
    IslandBuiltGraphStats stats{};

    bool has_unsafe_refs() const {
        return stats.outOfRangeBodyIndexCount > 0u || stats.outOfRangeContactRefCount > 0u ||
               stats.outOfRangeDistanceRefCount > 0u;
    }

    bool is_consistent() const { return !skipped && !has_unsafe_refs(); }

/// Aggregate solveable counts for graph-level batch guards.
struct IslandSolveableStats {
    u32 solveableCount = 0;
    u32 allSleepingCount = 0;
    u32 noMovableBodiesCount = 0;
    u32 staleRefsCount = 0;

/// Graph-level solveable preflight combining dispatch, sleep, and constraint coverage.
struct IslandSolveableGraphPreflight {
    IslandSolveableStats stats{};

    bool has_solveable() const { return !skipped && stats.solveableCount > 0u; }
    bool can_dispatch() const { return !skipped && reason == IslandDispatchRejectReason::None; }
    bool can_dispatch() const { return reason == IslandDispatchRejectReason::None; }
    bool noDispatchableIslands = false;

};

/// Why one island constraint solve would early-out (B4.4 deepen pass).
enum class IslandSolveRejectReason : u8 {
    None = 0,
    EmptyIsland,
    InvalidDt,
    NoInRangeRefs,
    NoMovableBodies,

/// Human-readable label for island solve reject reasons (logging / tests).
const char* islandSolveRejectReasonName(IslandSolveRejectReason reason);

/// Diagnose why one island job would skip solve dispatch.
IslandSolveRejectReason islandSolveJobRejectReason(const IslandSolveJob& job, f32 dt);

/// Diagnose why one island constraint solve would skip.
IslandSolveRejectReason islandConstraintSolveRejectReason(
    const ContactIslandGraph::Island& island,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints);

/// Returns true when `islandSolveJobRejectReason` matches `expected` (B4.4 deepen pass).
bool islandSolveRejectsForReason(const IslandSolveJob& job,
                                 f32 dt,
                                 IslandSolveRejectReason expected);

/// Returns true when `islandConstraintSolveRejectReason` matches `expected` (B4.4 deepen pass).
bool islandConstraintSolveRejectsForReason(const ContactIslandGraph::Island& island,
                                           const std::vector<DistanceConstraint>& distanceConstraints,
/// Diagnose why island batch dispatch would skip.
IslandDispatchRejectReason islandDispatchRejectReason(const IslandDispatchPreflight& preflight);

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

/// Batch dispatch summary with sleep-skip counts (B4.4 deepen follow-up pass).
struct IslandBatchSleepDispatchResult {
    u32 solvedCount = 0;
    u32 skippedCount = 0;
    u32 sleepingSkippedCount = 0;
    u32 dispatchableCount = 0;
    bool skipped = false;

    bool any_solved() const { return solvedCount > 0u; }
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

/// Constraint index coverage for one island solve pass (stale/out-of-range guards).
struct IslandConstraintRefsPreflight {
    u32 inRangeContactCount = 0;
    u32 inRangeDistanceCount = 0;
    u32 solvableContactCount = 0;
    u32 solvableDistanceCount = 0;

        return !skipped && (inRangeContactCount > 0u || inRangeDistanceCount > 0u);

/// Input coverage for island graph build (out-of-range body-index guards).
struct IslandBuildStats {
    u32 outOfRangeContactBodyCount = 0;
    u32 outOfRangeDistanceBodyCount = 0;

/// Preflight diagnostics for island graph build inputs (B4.4 deepen).
    ContactIslandGraphBuildRejectReason reason = ContactIslandGraphBuildRejectReason::None;
    IslandBuildStats stats{};

    bool has_unsafe_refs() const {
        return stats.outOfRangeContactBodyCount > 0u || stats.outOfRangeDistanceBodyCount > 0u;

    bool can_build() const { return reason == ContactIslandGraphBuildRejectReason::None; }

/// Body participation for one island constraint solve (sleep/static/movable guards).
    u32 movableCount = 0;

    bool can_solve() const { return !skipped && movableCount > 0u; }

/// Combined constraint-ref + body participation preflight for one island solve pass.
struct IslandConstraintSolvePreflight {
    IslandConstraintRefsPreflight refs{};
    IslandSolveBodiesPreflight bodies{};

    bool can_solve() const { return !skipped && refs.can_solve() && bodies.can_solve(); }

/// Per-island sleep state for solve early-out stubs.
    u32 activeDynamicCount = 0;


/// Per-island wake hint when active dynamics neighbor sleeping bodies.
    bool hasMixedSleepState = false;

    bool should_wake_sleepers() const {
        return !skipped && hasMixedSleepState && activeDynamicCount > 0u;

/// Aggregate sleep counts for graph-level batch guards.
    u32 mixedSleepCount = 0;
    u32 fullyActiveCount = 0;


    bool has_solveable_islands() const {
        return !skipped && (stats.fullyActiveCount > 0u || stats.mixedSleepCount > 0u);

/// Aggregate wake counts for graph-level batch guards.
struct IslandWakeGraphStats {
    u32 wakeableCount = 0;

/// Graph-level wake preflight for selective per-island sleeper activation.
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
    IslandConstraintRefsPreflight constraintRefs{};

        return !skipped && job.can_dispatch() && constraintRefs.can_solve() && sleep.can_solve();

    bool has_solvable_constraints() const {
        return !skipped && (solvableContactCount > 0u || solvableDistanceCount > 0u);

/// Per-island sleep/wake preflight for selective solve early-out (B4.4 deepen).
    u32 inactiveCount = 0;
    bool allInactive = false;

    bool can_solve() const { return !skipped && activeDynamicCount > 0u; }

    u32 allInactiveCount = 0;


    bool can_dispatch() const { return !skipped && stats.activeCount > 0u; }

/// Combined constraint-ref and sleep/wake preflight for one island solve pass.
    IslandSleepWakePreflight sleepWake{};

        return !skipped && sleepWake.can_solve() && constraintRefs.has_solvable_constraints();

/// Combined dispatch preflight (graph + timestep + sleep/wake guards).
struct IslandDispatchBodiesPreflight {
    IslandSleepWakeGraphPreflight sleepWake{};

        return !skipped && dispatch.can_dispatch() && sleepWake.can_dispatch();

/// Combined constraint solve preflight (refs + bodies + sleep + dt guards).
    IslandBodyRefsPreflight bodies{};

        return !skipped && !invalidDt && refs.can_solve() && bodies.can_solve() && sleep.can_solve();

/// Per-island body state scan for sleep/wake solve guards (B4.4 deepen follow-up pass).

        return !skipped && inRangeBodyCount > 0u && sleepingCount == inRangeBodyCount;

    bool all_static_or_sleeping() const {
        return !skipped && inRangeBodyCount > 0u &&
               (staticOrKinematicCount + sleepingCount) == inRangeBodyCount;

    bool has_awake_dynamic() const { return awakeDynamicCount > 0u; }

    bool can_solve() const { return !skipped && has_awake_dynamic(); }

    bool should_remain_asleep() const { return all_sleeping(); }

    bool should_wake() const { return has_awake_dynamic(); }

/// Combined constraint-ref + sleep/wake preflight for one island solve pass.

    bool can_solve() const { return !skipped && refs.can_solve() && sleepWake.can_solve(); }
/// Per-island sleep/wake coverage for constraint solve dispatch (B4.4 deepen).

    bool is_all_sleeping() const { return bodyCount > 0u && sleepingCount == bodyCount; }
    bool has_wakeable_body() const { return wakeableCount > 0u; }

    bool can_solve() const { return !skipped && has_wakeable_body(); }





/// Body index coverage for one island solve pass (stale/out-of-range guards).

        return !skipped && ownedBodyCount > 0u && inRangeBodyCount == ownedBodyCount;

/// Combined constraint + body coverage for one island solve pass.
struct IslandSolveRefsPreflight {
    IslandConstraintRefsPreflight constraints{};

    bool can_solve() const { return !skipped && constraints.can_solve() && bodies.can_solve(); }

/// Per-island sleep aggregation for selective solve skip (B4.4 deepen follow-up).
    u32 dynamicAwakeCount = 0;

    bool all_dynamic_sleeping() const {
        return !skipped && dynamicAwakeCount == 0u && ownedBodyCount > staticOrKinematicCount;

    bool can_skip_solve() const { return all_dynamic_sleeping(); }

/// Per-island wake candidate scan for sleep/wake preflight (B4.4 deepen follow-up).
    u32 penetratingContactCount = 0;

        return !skipped &&
               (wakeCandidateCount > 0u || externalForceCount > 0u || penetratingContactCount > 0u);

/// Graph-level sleep aggregation for batch solve skip guards.
    u32 fullySleepingCount = 0;

/// Graph-level sleep preflight for selective island solve skip.

    bool all_fully_sleeping() const {
        return !skipped && stats.fullySleepingCount == stats.totalIslands && stats.totalIslands > 0u;

    bool can_skip_all() const { return all_fully_sleeping(); }

/// Graph-level wake preflight for batch wake guards.
    u32 wakeableIslandCount = 0;

    bool any_should_wake() const { return !skipped && wakeableIslandCount > 0u; }


/// Per-island sleep state for solve/wake guards (B4.4 deepen).


    bool can_solve_awake() const { return !skipped && !all_dynamic_sleeping(); }

/// Per-island wake hint when mixed sleep/awake bodies share an island (B4.4 deepen).

    bool needs_wake() const { return !skipped && sleepingCount > 0u && awakeDynamicCount > 0u; }

/// Aggregate sleep counts for graph-level dispatch guards.

/// Graph-level sleep preflight for selective per-island solve dispatch.

    bool can_dispatch_awake() const { return !skipped && stats.awakeCount > 0u; }

/// Combined solve preflight (constraint refs + body refs + sleep).
struct IslandSolvePassPreflight {
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

    bool can_dispatch() const { return !skipped && stats.wakeableCount > 0u; }
/// Sleep/wake preflight for one island constraint solve pass (B4.4 deepen follow-up).
struct IslandSleepSolvePreflight {

    bool can_solve() const { return !skipped && awakeBodyCount > 0u; }

/// Graph-level sleep preflight for island dispatch batching (B4.4 deepen follow-up).
    u32 sleepingOnlyCount = 0;

    bool can_dispatch() const { return !skipped && awakeCount > 0u; }

/// Wake preflight for sleeping islands with external activity (B4.4 deepen follow-up).

        return !skipped && sleepingDynamicCount > 0u &&
               (awakeDynamicCount > 0u || nonZeroImpulseCount > 0u);

struct IslandSolveBodyPreflight {
    IslandSleepSolvePreflight sleep{};


/// Combined constraint-solve preflight (dt + refs + sleep/wake guards).

        return !skipped && !invalidDt && refs.can_solve() && sleepWake.can_solve();

    IslandJobDispatchRejectReason reason = IslandJobDispatchRejectReason::None;
    IslandSolveRejectReason reason = IslandSolveRejectReason::None;

/// Human-readable label for island solve-job reject reasons (logging / tests).
const char* island_solve_job_reject_reason_name(IslandSolveJobRejectReason reason);

/// Diagnose why one island solve job would skip dispatch.
IslandSolveJobRejectReason island_solve_job_reject_reason(const IslandSolveJob& job, f32 dt);

/// Returns true when `island_solve_job_reject_reason` matches `expected`.
bool island_solve_job_rejects_for_reason(const IslandSolveJob& job,

/// Why per-job island solve dispatch would early-out (B4.4 deepen follow-up pass).

/// Human-readable label for per-job island solve reject reasons (B4.4 deepen follow-up pass).

    bool invalidDt = false;
    bool skipped = false;
    u32 constraintCount = 0;

    bool can_dispatch() const { return !skipped && reason == IslandJobDispatchRejectReason::None; }
    bool can_dispatch() const { return reason == IslandSolveJobRejectReason::None; }
    bool can_dispatch() const { return reason == IslandSolveRejectReason::None; }
    bool can_dispatch() const { return reason == IslandSolveRejectReason::None && !skipped && !invalidDt && constraintCount > 0u; }
    bool can_dispatch() const {
        return !skipped && reason == IslandSolveJobRejectReason::None && !invalidDt && constraintCount > 0u;
    }
};

/// Why island graph build would early-out (B4.4 deepen follow-up pass).
enum class IslandBuildRejectReason : u8 {
    None = 0,
    EmptyInput,
    OutOfRangeRefs,
    DegenerateRefs,
};

/// Human-readable label for island-build reject reasons (B4.4 deepen follow-up pass).
const char* island_build_reject_reason_name(IslandBuildRejectReason reason);

/// Diagnose why island graph build would skip; vacuously succeeds when build may proceed.
IslandBuildRejectReason island_build_reject_reason(
    u32 bodyCount,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints);

/// Returns true when `island_build_reject_reason` matches `expected`.
bool island_build_rejects_for_reason(
    const std::vector<DistanceConstraint>& distanceConstraints,
    IslandBuildRejectReason expected);

/// Why per-island constraint solve would early-out (B4.4 deepen follow-up pass).
enum class IslandConstraintSolveRejectReason : u8 {
    EmptyIsland,
    NoInRangeRefs,
    NoMovableBodies,

/// Human-readable label for island constraint-solve reject reasons (B4.4 deepen follow-up pass).
const char* island_constraint_solve_reject_reason_name(IslandConstraintSolveRejectReason reason);

/// Diagnose why one island constraint solve would skip.
IslandConstraintSolveRejectReason island_constraint_solve_reject_reason(
    const ContactIslandGraph::Island& island,
    const RigidBodySoA& bodies,

/// Returns true when `island_constraint_solve_reject_reason` matches `expected`.
bool island_constraint_solve_rejects_for_reason(
    IslandConstraintSolveRejectReason expected);

/// Why per-island sleep solve would early-out (B4.4 deepen follow-up pass).
enum class IslandSleepRejectReason : u8 {
    AllSleeping,

/// Human-readable label for island sleep reject reasons (B4.4 deepen follow-up pass).
const char* island_sleep_reject_reason_name(IslandSleepRejectReason reason);

/// Diagnose why one island sleep preflight would skip solve.
IslandSleepRejectReason island_sleep_reject_reason(const ContactIslandGraph::Island& island,
                                                  const RigidBodySoA& bodies);

/// Returns true when `island_sleep_reject_reason` matches `expected`.
bool island_sleep_rejects_for_reason(const ContactIslandGraph::Island& island,
                                     IslandSleepRejectReason expected);

/// Why per-island wake would early-out (B4.4 deepen follow-up pass).
enum class IslandWakeRejectReason : u8 {
    NoWakeTarget,

/// Human-readable label for island wake reject reasons (B4.4 deepen follow-up pass).
const char* island_wake_reject_reason_name(IslandWakeRejectReason reason);

/// Diagnose why one island wake preflight would skip sleeper activation.
IslandWakeRejectReason island_wake_reject_reason(const ContactIslandGraph::Island& island,

/// Returns true when `island_wake_reject_reason` matches `expected`.
bool island_wake_rejects_for_reason(const ContactIslandGraph::Island& island,
                                    IslandWakeRejectReason expected);

/// Why graph-level sleep batching would early-out (B4.4 deepen follow-up pass).
enum class IslandSleepGraphRejectReason : u8 {
    EmptyGraph,
    AllIslandsSleeping,

/// Human-readable label for graph sleep reject reasons (B4.4 deepen follow-up pass).
const char* island_sleep_graph_reject_reason_name(IslandSleepGraphRejectReason reason);

/// Diagnose why graph-level sleep solve batch would skip.
IslandSleepGraphRejectReason island_sleep_graph_reject_reason(const ContactIslandGraph& graph,

/// Returns true when `island_sleep_graph_reject_reason` matches `expected`.
bool island_sleep_graph_rejects_for_reason(const ContactIslandGraph& graph,
                                           IslandSleepGraphRejectReason expected);

/// Why graph-level wake batching would early-out (B4.4 deepen follow-up pass).
enum class IslandWakeGraphRejectReason : u8 {
    NoWakeableIslands,

/// Human-readable label for graph wake reject reasons (B4.4 deepen follow-up pass).
const char* island_wake_graph_reject_reason_name(IslandWakeGraphRejectReason reason);

/// Diagnose why graph-level wake batch would skip.
IslandWakeGraphRejectReason island_wake_graph_reject_reason(const ContactIslandGraph& graph,

/// Returns true when `island_wake_graph_reject_reason` matches `expected`.
bool island_wake_graph_rejects_for_reason(const ContactIslandGraph& graph,
                                          IslandWakeGraphRejectReason expected);
/// Diagnose why one island solve job would skip.
IslandSolveJobRejectReason islandSolveJobRejectReason(const IslandSolveJobPreflight& preflight);

/// Constraint index coverage for one island solve pass (stale/out-of-range guards).
struct IslandConstraintRefsPreflight {
    IslandConstraintRefsRejectReason reason = IslandConstraintRefsRejectReason::None;
    IslandSolveRejectReason reason = IslandSolveRejectReason::None;
    u32 ownedContactCount = 0;
    u32 ownedDistanceCount = 0;
    u32 inRangeContactCount = 0;
    u32 inRangeDistanceCount = 0;
    u32 outOfRangeContactCount = 0;
    u32 outOfRangeDistanceCount = 0;
    u32 validContactCount = 0;
    u32 inRangeContactBodyCount = 0;
    u32 outOfRangeContactBodyCount = 0;
    u32 inRangeDistanceBodyCount = 0;
    u32 outOfRangeDistanceBodyCount = 0;
    bool skipped = false;

    bool has_unsafe_body_refs() const {
        return outOfRangeContactBodyCount > 0u || outOfRangeDistanceBodyCount > 0u;
    bool can_solve() const {
        return reason == IslandSolveRejectReason::None && !skipped &&
               (inRangeContactCount > 0u || inRangeDistanceCount > 0u);
    }

    bool can_solve() const {
        return !skipped && !has_unsafe_body_refs() &&
               (inRangeContactCount > 0u || inRangeDistanceCount > 0u);
    bool can_solve() const { return reason == IslandConstraintRefsRejectReason::None; }
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
    OutOfRangeContact,
    OutOfRangeDistance,

/// Human-readable label for island build reject reasons (B4.4 deepen follow-up pass).

/// Returns the first reject reason for build inputs, or `None` when build may proceed.


/// Diagnose why island graph build would skip; vacuously succeeds when build may proceed (B4.4 deepen follow-up pass).
/// Human-readable label for island-build reject reasons (logging / tests).

/// Diagnose why island graph build would reject; vacuously succeeds when build may proceed.
IslandBuildRejectReason island_build_reject_reason(
    u32 bodyCount,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints);

/// Returns true when `island_build_reject_reason` matches `expected` (B4.5 deepen follow-up pass).
/// Returns true when `island_build_reject_reason` matches `expected`.
/// Returns true when `island_build_reject_reason` matches `expected` (B4.4 deepen follow-up pass).
bool island_build_rejects_for_reason(
    const std::vector<DistanceConstraint>& distanceConstraints,
    IslandBuildRejectReason expected);

/// Why island constraint solve would early-out (B4.5 deepen follow-up pass).
enum class IslandConstraintSolveRejectReason : u8 {
    EmptyIsland,
    OutOfRangeIslandIndex,
    StaleConstraintRefs,
    NoMovableBodies,

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

/// Diagnose why constraint solve by island index would skip (B4.5 deepen follow-up pass).
IslandConstraintSolveRejectReason island_constraint_solve_reject_reason_by_index(
    const ContactIslandGraph& graph,
    u32 islandIndex,

/// Returns true when `island_constraint_solve_reject_reason` matches `expected` (B4.5 deepen follow-up pass).
bool island_constraint_solve_rejects_for_reason(
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

/// Why per-island sleep solve would early-out (B4.4 deepen follow-up pass).

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
bool island_build_rejects_for_reason(u32 bodyCount,

    u32 bodyCount,
    const std::vector<narrowphase::ContactManifold>& contacts,

/// Human-readable label for island-dispatch reject reasons (logging / tests).
const char* island_dispatch_reject_reason_name(IslandDispatchRejectReason reason);

/// Diagnose why graph-level island dispatch would skip; vacuously succeeds when dispatch may proceed.
IslandDispatchRejectReason island_dispatch_reject_reason(const ContactIslandGraph& graph, f32 dt);

/// Returns true when `island_dispatch_reject_reason` matches `expected` (B4.4 deepen follow-up pass).
bool island_dispatch_rejects_for_reason(
    f32 dt,
    IslandDispatchRejectReason expected);

/// Human-readable label for island-solve-job reject reasons (logging / tests).
const char* island_solve_job_reject_reason_name(IslandSolveJobRejectReason reason);

/// Diagnose why one island solve job would skip; vacuously succeeds when dispatch may proceed.
IslandSolveJobRejectReason island_solve_job_reject_reason(const IslandSolveJob& job, f32 dt);

/// Returns true when `island_solve_job_reject_reason` matches `expected` (B4.4 deepen follow-up pass).
bool island_solve_job_rejects_for_reason(
    const IslandSolveJob& job,
    IslandSolveJobRejectReason expected);

/// Human-readable label for island-constraint-solve reject reasons (logging / tests).

/// Diagnose why one island constraint solve would skip; vacuously succeeds when solve may proceed.
    const std::vector<DistanceConstraint>& distanceConstraints);

/// Returns true when `island_constraint_solve_reject_reason` matches `expected` (B4.4 deepen follow-up pass).

/// Human-readable label for island-sleep reject reasons (logging / tests).

/// Diagnose why one island sleep preflight would skip solve; vacuously succeeds when solve may proceed.

/// Returns true when `island_sleep_reject_reason` matches `expected` (B4.4 deepen follow-up pass).

/// Human-readable label for island-wake reject reasons (logging / tests).

/// Diagnose why one island wake would skip; vacuously succeeds when wake may proceed.

/// Returns true when `island_wake_reject_reason` matches `expected` (B4.4 deepen follow-up pass).

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
    u32 invalidContactCount = 0;
    u32 selfDistanceCount = 0;
    u32 selfReferentialContactCount = 0;
    u32 selfReferentialDistanceCount = 0;
using IslandBuildStats = IslandBuildInputScan;

/// Explicit outcome for guarded island graph build (B4.4 deepen).
struct IslandBuildResult {
    bool built = false;
    bool skipped = false;
    bool unsafeRefs = false;
    IslandBuildStats stats{};

/// Why island constraint solve would reject one island (B4.4 deepen follow-up pass).
enum class IslandConstraintSolveRejectReason : u8 {
    None = 0,
    EmptyIsland,
    StaleRefs,
    NoMovableBodies,
    AllSleeping,
};
using IslandBuildStats = IslandGraphBuildStats;

/// Human-readable label for island constraint-solve reject reasons (B4.4 deepen follow-up pass).
const char* island_constraint_solve_reject_reason_name(IslandConstraintSolveRejectReason reason);

/// Diagnose why one island constraint solve would skip.
IslandConstraintSolveRejectReason island_constraint_solve_reject_reason(
    const ContactIslandGraph::Island& island,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints);

/// Returns true when `island_constraint_solve_reject_reason` matches `expected`.
bool island_constraint_solve_rejects_for_reason(
    const ContactIslandGraph::Island& island,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints,
    IslandConstraintSolveRejectReason expected);

/// Why island sleep preflight would skip solve for one island (B4.4 deepen follow-up pass).
enum class IslandSleepSolveRejectReason : u8 {
    None = 0,
    EmptyIsland,
    AllSleeping,
};

/// Human-readable label for island sleep-solve reject reasons (B4.4 deepen follow-up pass).
const char* island_sleep_solve_reject_reason_name(IslandSleepSolveRejectReason reason);

/// Diagnose why one island is eligible for sleep-solve early-out.
IslandSleepSolveRejectReason island_sleep_solve_reject_reason(const ContactIslandGraph::Island& island,
                                                              const RigidBodySoA& bodies);

/// Returns true when `island_sleep_solve_reject_reason` matches `expected`.
bool island_sleep_solve_rejects_for_reason(const ContactIslandGraph::Island& island,
                                           const RigidBodySoA& bodies,
                                           IslandSleepSolveRejectReason expected);

/// Why island wake preflight would skip sleeper activation (B4.4 deepen follow-up pass).
enum class IslandWakeRejectReason : u8 {
    None = 0,
    EmptyIsland,
    NoActiveDynamic,
    UniformSleepState,
};

/// Human-readable label for island wake reject reasons (B4.4 deepen follow-up pass).
const char* island_wake_reject_reason_name(IslandWakeRejectReason reason);

/// Diagnose why one island does not need sleeper activation.
IslandWakeRejectReason island_wake_reject_reason(const ContactIslandGraph::Island& island,
                                                 const RigidBodySoA& bodies);

/// Returns true when `island_wake_reject_reason` matches `expected`.
bool island_wake_rejects_for_reason(const ContactIslandGraph::Island& island,
                                    const RigidBodySoA& bodies,
                                    IslandWakeRejectReason expected);

/// Preflight diagnostics for island graph build inputs (B4.4 deepen).
struct IslandBuildPreflight {
    IslandBuildRejectReason reason = IslandBuildRejectReason::None;
    IslandGraphBuildRejectReason reason = IslandGraphBuildRejectReason::None;
    IslandBuildStats stats{};
    IslandGraphBuildRejectReason reason = IslandGraphBuildRejectReason::None;
    IslandBuildRejectReason reason = IslandBuildRejectReason::None;
    bool skipped = false;
    bool emptyInput = false;
    bool outOfRangeRefs = false;

    bool has_unsafe_refs() const {
        return stats.outOfRangeContactBodyCount > 0u || stats.outOfRangeDistanceBodyCount > 0u;

    bool has_degenerate_refs() const { return stats.selfContactCount > 0u; }

    bool can_build() const { return !skipped && !has_unsafe_refs() && !has_degenerate_refs(); }
    bool can_build() const { return reason == IslandBuildRejectReason::None && !skipped && !has_unsafe_refs(); }
    bool has_degenerate_refs() const {
        return stats.selfPairContactCount > 0u || stats.selfPairDistanceCount > 0u;
        return stats.outOfRangeContactBodyCount > 0u || stats.outOfRangeDistanceBodyCount > 0u ||
               stats.selfReferentialContactCount > 0u || stats.selfReferentialDistanceCount > 0u;
    }

    bool can_build() const { return reason == IslandBuildRejectReason::None; }
    bool can_build() const { return !skipped && reason == IslandBuildRejectReason::None && !has_unsafe_refs(); }
    bool can_build() const { return !skipped && reason == IslandBuildRejectReason::None; }
        return stats.selfContactCount > 0u || stats.selfDistanceCount > 0u;

    bool can_build() const {
        return reason == IslandBuildRejectReason::None && !skipped && !has_unsafe_refs();
using IslandBuildStats = IslandGraphBuildStats;
using IslandBuildPreflight = IslandGraphBuildPreflight;

/// Explicit outcome for guarded island graph build.
/// Guarded island graph build outcome (skip vs build).

    bool can_build() const { return !skipped && reason == IslandGraphBuildRejectReason::None; }
    bool can_build() const { return reason == IslandBuildRejectReason::None && !skipped && !has_unsafe_refs(); }
    bool can_build() const { return reason == IslandGraphBuildRejectReason::None; }
        return !skipped && reason == IslandGraphBuildRejectReason::None && !has_unsafe_refs();
    }
    bool can_build() const { return reason == IslandGraphBuildRejectReason::None && !skipped && !has_unsafe_refs(); }
};

/// Outcome for guarded island graph build (skip vs partition).
using IslandBuildOutcome = IslandGraphBuildOutcome;

struct IslandBuildResult {
    bool built = false;
    bool skipped = false;
    bool unsafeRefs = false;
    bool degenerateRefs = false;
    IslandBuildStats stats{};
        return !skipped && reason == IslandGraphBuildRejectReason::None && !has_unsafe_refs();
    }
};




/// Guarded island graph build outcome (B4.4 deepen follow-up).
    IslandBuildRejectReason reason = IslandBuildRejectReason::None;
/// Why island graph build would reject (B4.4 deepen follow-up pass).
enum class IslandBuildRejectReason : u8 {
    None = 0,
    EmptyInputs,
    OutOfRangeContactBodies,
    OutOfRangeDistanceBodies,

/// Human-readable label for island build reject reasons (logging / tests).
const char* islandBuildRejectReasonName(IslandBuildRejectReason reason);

/// Diagnose why build would reject; vacuously succeeds when build may proceed.
IslandBuildRejectReason island_build_reject_reason(
    EmptyInput,
    OutOfRangeContactRef,
    OutOfRangeDistanceRef,

/// Human-readable label for island-build reject reasons (logging / tests).

/// Diagnose why build would skip; vacuously succeeds when build may proceed.
IslandBuildRejectReason islandBuildRejectReason(
    u32 bodyCount,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints);

/// Returns true when `island_build_reject_reason` matches `expected` (B4.4 deepen follow-up pass).
bool island_build_rejects_for_reason(
    const std::vector<DistanceConstraint>& distanceConstraints,
    IslandBuildRejectReason expected);

/// Read-only island build diagnostics with reject reason — no mutation (B4.4 deepen follow-up).
struct IslandBuildDeepenPreflight {
    bool rejected = false;

    bool can_build() const { return !rejected; }

/// Populate island build deepen preflight without mutating the graph (B4.4 deepen follow-up).
IslandBuildDeepenPreflight preflight_island_build_deepen(

/// Returns true when island graph build should be skipped (B4.4 deepen follow-up).
bool should_skip_island_build_deepen(

/// Non-mutating build predicate — inverse of `should_skip_island_build_deepen` (B4.4 deepen follow-up).
bool should_run_island_build(

/// Outcome of a guarded island graph build (B4.4 deepen).
    IslandBuildPreflight preflight{};


/// Per-island constraint solve outcome (skip vs solve).
struct IslandConstraintSolveResult {
    bool solved = false;
    u32 islandIndex = ContactIslandGraph::invalidIsland;

/// Per-island wake outcome (skip vs activated).
struct IslandWakeResult {
    bool woke = false;

/// Batch wake summary for graph-level sleeper activation.
struct IslandBatchWakeResult {
    u32 wokeCount = 0;
    u32 skippedCount = 0;
    u32 wakeableCount = 0;

    bool any_woke() const { return wokeCount > 0u; }
/// Non-mutating build skip predicate — inverse of `can_build` (B4.4 deepen follow-up pass).
bool can_skip_island_build_for_reason(u32 bodyCount,

/// Non-mutating build predicate — mirrors `preflight_island_build` (B4.4 deepen follow-up pass).
bool should_run_island_build(u32 bodyCount,
/// Returns true when `islandBuildRejectReason` matches `expected` (B4.4 deepen follow-up pass).
bool islandBuildRejectsForReason(u32 bodyCount,

/// Read-only island-build diagnostics with reject reason — no mutation (B4.4 deepen follow-up).
struct IslandBuildRejectPreflight {
    IslandBuildPreflight build{};
    bool emptyInput = false;
    bool unsafeContactRefs = false;
    bool unsafeDistanceRefs = false;

    bool can_build() const { return reason == IslandBuildRejectReason::None && build.can_build(); }

IslandBuildRejectPreflight preflight_island_build_reject(

bool canSkipIslandBuild(u32 bodyCount,

/// Non-mutating build predicate — mirrors `preflight_island_build_reject` (B4.4 deepen follow-up pass).
bool shouldRunIslandBuild(u32 bodyCount,
/// Why island constraint solve would reject (B4.4 deepen pass).
enum class IslandSolveRejectReason : u8 {
    EmptyIsland,
    OutOfRangeRefs,
    NoMovableBodies,

/// Human-readable label for island constraint solve reject reasons (logging / tests).
const char* islandSolveRejectReasonName(IslandSolveRejectReason reason);

/// Body participation for one island constraint solve (sleep/static/movable guards).
struct IslandSolveBodiesPreflight {
    IslandSolveRejectReason reason = IslandSolveRejectReason::None;
    u32 bodyCount = 0;
    u32 inRangeBodyCount = 0;
    u32 outOfRangeBodyCount = 0;
    u32 staticOrKinematicCount = 0;
    u32 sleepingCount = 0;
    u32 movableCount = 0;

    bool has_unsafe_body_refs() const { return outOfRangeBodyCount > 0u; }

    bool can_solve() const { return !skipped && movableCount > 0u; }

/// Why per-island constraint solve would early-out (B4.5 deepen follow-up pass).
enum class IslandConstraintSolveRejectReason : u8 {
    None = 0,
    EmptyIsland,
    StaleConstraintRefs,
    AllSleeping,
    NoMovableBodies,
    bool can_solve() const { return reason == IslandSolveRejectReason::None && !skipped && movableCount > 0u; }
};

/// Human-readable label for island constraint-solve reject reasons (B4.5 deepen follow-up pass).
const char* island_constraint_solve_reject_reason_name(IslandConstraintSolveRejectReason reason);

/// Diagnose why island constraint solve would skip; vacuously succeeds when solve may proceed (B4.5 deepen follow-up pass).
/// Why island constraint solve would early-out (B4.4 deepen follow-up pass).
/// Why one island constraint solve would early-out (B4.4 deepen follow-up pass).
enum class IslandConstraintSolveRejectReason : u8 {
    None = 0,
    EmptyIsland,
    StaleRefs,
    NoMovableBodies,
};

/// Human-readable label for island constraint solve reject reasons (B4.4 deepen follow-up pass).

/// Diagnose why island constraint solve would skip; vacuously succeeds when solve may proceed (B4.4 deepen follow-up pass).
    StaleConstraintRefs,

/// Human-readable label for island constraint solve reject reasons (logging / tests).

/// Diagnose why island constraint solve would skip; vacuously succeeds when solve may proceed.
IslandConstraintSolveRejectReason island_constraint_solve_reject_reason(
/// Why one island constraint solve would skip (B4.4 deepen pass).

const char* islandConstraintSolveRejectReasonName(IslandConstraintSolveRejectReason reason);

/// Diagnose why one island constraint solve would skip; vacuously succeeds when solve may proceed.
IslandConstraintSolveRejectReason islandConstraintSolveRejectReason(
    AllSleeping,

/// Human-readable label for island constraint-solve reject reasons (logging / tests).

/// Diagnose why one island constraint solve would skip.
    const ContactIslandGraph::Island& island,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints);

/// Returns true when `island_constraint_solve_reject_reason` matches `expected` (B4.5 deepen follow-up pass).
bool island_constraint_solve_rejects_for_reason(
    const std::vector<DistanceConstraint>& distanceConstraints,
    IslandConstraintSolveRejectReason expected);

    NoInRangeRefs,
    InvalidDt,


/// Returns true when `island_constraint_solve_reject_reason` matches `expected` (B4.4 deepen follow-up pass).
/// Returns true when `island_constraint_solve_reject_reason` matches `expected`.
/// Returns true when `islandConstraintSolveRejectReason` matches `expected` (B4.4 deepen pass).
bool islandConstraintSolveRejectsForReason(
/// Why one island constraint solve pass would early-out (B4.4 deepen follow-up pass).


/// Combined constraint-ref + body participation preflight for one island solve pass.
struct IslandConstraintSolvePreflight {
    IslandConstraintSolveRejectReason reason = IslandConstraintSolveRejectReason::None;
    IslandSolveRejectReason reason = IslandSolveRejectReason::None;
    IslandConstraintRefsPreflight refs{};
    IslandSolveBodiesPreflight bodies{};
    IslandConstraintSolveRejectReason reason = IslandConstraintSolveRejectReason::None;
    IslandSolveRejectReason reason = IslandSolveRejectReason::None;
    bool skipped = false;

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
    bool can_solve() const {
        return !skipped && reason == IslandConstraintSolveRejectReason::None && refs.can_solve() &&
               bodies.can_solve();
    }
    bool can_solve() const { return reason == IslandSolveRejectReason::None && !skipped && refs.can_solve() && bodies.can_solve(); }
};

/// Why island sleep preflight would skip solve (B4.4 deepen pass).
enum class IslandSleepRejectReason : u8 {
/// Why per-island sleep solve would early-out (B4.4 deepen follow-up pass).
enum class IslandSleepSolveRejectReason : u8 {
    None = 0,
    EmptyIsland,
    AllSleeping,

/// Human-readable label for island sleep reject reasons (logging / tests).
const char* islandSleepRejectReasonName(IslandSleepRejectReason reason);

/// Why graph-level island sleep batch would skip solve (B4.4 deepen pass).
enum class IslandSleepGraphRejectReason : u8 {
    AllSleepingOrEmpty,

/// Human-readable label for graph-level island sleep reject reasons (logging / tests).
const char* islandSleepGraphRejectReasonName(IslandSleepGraphRejectReason reason);
/// Diagnose why island constraint solve would skip.
IslandConstraintSolveRejectReason islandConstraintSolveRejectReason(
    const IslandConstraintSolvePreflight& preflight);

/// Why per-island sleep preflight would early-out (B4.4 deepen follow-up pass).
    OutOfRangeIndex,

/// Human-readable label for island sleep reject reasons (B4.4 deepen follow-up pass).
const char* island_sleep_reject_reason_name(IslandSleepRejectReason reason);

/// Per-island sleep state for solve early-out stubs.
struct IslandSleepPreflight {
    IslandSleepRejectReason reason = IslandSleepRejectReason::None;
/// Human-readable label for island sleep-solve reject reasons (logging / tests).
const char* island_sleep_solve_reject_reason_name(IslandSleepSolveRejectReason reason);

/// Diagnose why one island is eligible for sleep-solve early-out.
IslandSleepSolveRejectReason island_sleep_solve_reject_reason(const ContactIslandGraph::Island& island,
                                                              const RigidBodySoA& bodies);

/// Returns true when `island_sleep_solve_reject_reason` matches `expected`.
bool island_sleep_solve_rejects_for_reason(const ContactIslandGraph::Island& island,
                                           const RigidBodySoA& bodies,
                                           IslandSleepSolveRejectReason expected);

    IslandSleepSolveRejectReason reason = IslandSleepSolveRejectReason::None;
    u32 bodyCount = 0;
    u32 sleepingCount = 0;
    u32 staticOrKinematicCount = 0;
    u32 activeDynamicCount = 0;
    IslandSleepRejectReason reason = IslandSleepRejectReason::None;
    bool allSleeping = false;

    bool can_skip_solve() const { return reason == IslandSleepRejectReason::AllSleeping; }

/// Why island wake preflight would skip activation (B4.4 deepen pass).
enum class IslandWakeRejectReason : u8 {
    None = 0,
    EmptyIsland,
    NoWakeTarget,
    bool can_skip_solve() const { return reason == IslandSleepRejectReason::None && !skipped && allSleeping; }
};

/// Human-readable label for island wake reject reasons (logging / tests).
const char* islandWakeRejectReasonName(IslandWakeRejectReason reason);

/// Why graph-level island wake batch would skip activation (B4.4 deepen pass).
enum class IslandWakeGraphRejectReason : u8 {
    NoWakeableIslands,

/// Human-readable label for graph-level island wake reject reasons (logging / tests).
const char* islandWakeGraphRejectReasonName(IslandWakeGraphRejectReason reason);
    bool can_skip_solve() const {
        return !skipped &&
               (reason == IslandSleepSolveRejectReason::AllSleeping || allSleeping);
    }

/// Why per-island wake would early-out (B4.4 deepen follow-up pass).
    NoActiveDynamic,
    UniformSleepState,

const char* island_wake_reject_reason_name(IslandWakeRejectReason reason);

/// Diagnose why one island does not need sleeper activation.
IslandWakeRejectReason island_wake_reject_reason(const ContactIslandGraph::Island& island,
                                                 const RigidBodySoA& bodies);

/// Returns true when `island_wake_reject_reason` matches `expected`.
bool island_wake_rejects_for_reason(const ContactIslandGraph::Island& island,
                                    const RigidBodySoA& bodies,
                                    IslandWakeRejectReason expected);
/// Diagnose why per-island sleep preflight would skip solve.
IslandSleepRejectReason islandSleepRejectReason(const IslandSleepPreflight& preflight);

    bool can_skip_solve() const { return !skipped && reason == IslandSleepRejectReason::None && allSleeping; }

/// Why per-island wake preflight would early-out (B4.4 deepen follow-up pass).
    OutOfRangeIndex,
    NoMixedSleepState,

/// Human-readable label for island wake reject reasons (B4.4 deepen follow-up pass).

/// Per-island wake hint when active dynamics neighbor sleeping bodies.
struct IslandWakePreflight {
    IslandWakeRejectReason reason = IslandWakeRejectReason::None;
    u32 bodyCount = 0;
    u32 sleepingCount = 0;
    u32 activeDynamicCount = 0;
    IslandWakeRejectReason reason = IslandWakeRejectReason::None;
    bool hasMixedSleepState = false;

    bool should_wake_sleepers() const { return reason == IslandWakeRejectReason::None; }
};

/// Aggregate constraint-solve participation counts for graph-level batch guards.
    u32 staleRefsCount = 0;

/// Graph-level constraint solve preflight for selective per-island dispatch.

    bool can_dispatch() const { return !skipped && stats.solveableCount > 0u; }

/// Why one island constraint solve would reject (B4.4 deepen follow-up pass).
enum class IslandConstraintSolveRejectReason : u8 {
    None = 0,
    EmptyIsland,
    NoInRangeConstraints,
    NoMovableBodies,

/// Human-readable label for island constraint solve reject reasons (logging / tests).
const char* islandConstraintSolveRejectReasonName(IslandConstraintSolveRejectReason reason);

/// Diagnose why constraint solve would reject; vacuously succeeds when solve may proceed.
/// Why island constraint solve would early-out (B4.4 deepen follow-up pass).
    StaleRefs,
    AllSleeping,

/// Human-readable label for island constraint-solve reject reasons (B4.4 deepen follow-up pass).
const char* island_constraint_solve_reject_reason_name(IslandConstraintSolveRejectReason reason);

/// Diagnose why constraint solve would skip; vacuously succeeds when solve may proceed (B4.4 deepen follow-up pass).
IslandConstraintSolveRejectReason island_constraint_solve_reject_reason(
/// Why island constraint solve would reject (B4.4 deepen follow-up pass).

/// Human-readable label for island constraint-solve reject reasons (logging / tests).

/// Diagnose why constraint solve would skip; vacuously succeeds when solve may proceed.
IslandConstraintSolveRejectReason islandConstraintSolveRejectReason(
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

    bool can_solve() const { return !skipped && job.can_dispatch() && solve.can_solve(); }

/// Per-island wake outcome for parallel batch stubs (B4.4 deepen).
struct IslandWakeResult {
    bool woke = false;
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

    bool any_solved() const { return solvedCount > 0u; }



/// Graph-level constraint-solve preflight for selective per-island dispatch.

    bool can_solve_graph() const { return !skipped && stats.solveableCount > 0u; }
/// Per-island wake outcome (skip vs activate) for batch stubs.

/// Wake-then-solve outcome for one island dispatch stub.
struct IslandWakeAndSolveResult {

/// Graph-level wake-then-dispatch batch summary.
struct IslandWakeAndDispatchResult {
    IslandBatchDispatchResult dispatch{};

    bool any_solved() const { return dispatch.any_solved(); }

bool island_constraint_solve_reject_reason_is(


/// Graph-level constraint-solve preflight for selective per-island solve skipping.

/// Per-island guarded constraint-solve outcome (skip vs solve).
    bool has_solveable_islands() const { return !skipped && stats.solveableCount > 0u; }



        return reason == IslandConstraintSolveRejectReason::None && !skipped && refs.can_solve() &&

/// Why one island sleep preflight would early-out (B4.4 deepen follow-up pass).
enum class IslandSleepRejectReason : u8 {

/// Human-readable label for island sleep reject reasons (B4.4 deepen follow-up pass).
const char* island_sleep_reject_reason_name(IslandSleepRejectReason reason);

    u32 noMovableCount = 0;

    u32 noMovableBodiesCount = 0;
    u32 noInRangeRefsCount = 0;



    bool can_solve() const { return !skipped && stats.solveableCount > 0u; }
/// Per-island guarded constraint solve outcome (skip vs solve).

/// Combined job + constraint-solve preflight for guarded island dispatch.
struct IslandFullSolvePreflight {
    IslandConstraintSolvePreflight constraint{};

    bool can_solve() const { return !skipped && job.can_dispatch() && constraint.can_solve(); }




/// Combined constraint solve + timestep preflight for one island dispatch.
struct IslandConstraintSolveDispatchPreflight {
    bool invalidDt = false;

    bool can_solve() const { return !skipped && !invalidDt && solve.can_solve(); }

/// Non-mutating constraint-solve skip predicate — inverse of `can_solve` (B4.4 deepen follow-up pass).
bool can_skip_island_constraint_solve_for_reason(

/// Non-mutating constraint-solve predicate — mirrors `preflight_island_constraint_solve` (B4.4 deepen follow-up pass).

/// Why island sleep preflight would skip constraint solve (B4.4 deepen follow-up pass).


/// Diagnose why island sleep preflight would skip solve; vacuously succeeds when solve may proceed (B4.4 deepen follow-up pass).
IslandSleepRejectReason island_sleep_reject_reason(const ContactIslandGraph::Island& island,
                                                   const RigidBodySoA& bodies);

/// Returns true when `island_sleep_reject_reason` matches `expected` (B4.4 deepen follow-up pass).
bool island_sleep_rejects_for_reason(const ContactIslandGraph::Island& island,
                                     IslandSleepRejectReason expected);
/// Returns true when `islandConstraintSolveRejectReason` matches `expected` (B4.4 deepen follow-up pass).
bool islandConstraintSolveRejectsForReason(const ContactIslandGraph::Island& island,

/// Read-only constraint-solve diagnostics with reject reason — no mutation (B4.4 deepen follow-up).
struct IslandConstraintSolveRejectPreflight {
    bool emptyIsland = false;
    bool staleRefs = false;
    bool noMovableBodies = false;

    bool can_solve() const { return reason == IslandConstraintSolveRejectReason::None && solve.can_solve(); }

IslandConstraintSolveRejectPreflight preflight_island_constraint_solve_reject(

bool canSkipIslandConstraintSolve(const ContactIslandGraph::Island& island,

/// Non-mutating constraint-solve predicate — mirrors reject preflight (B4.4 deepen follow-up pass).
bool shouldRunIslandConstraintSolve(const ContactIslandGraph::Island& island,


    bool staleConstraintRefs = false;


    IslandSolveRejectReason reason = IslandSolveRejectReason::None;

    bool can_solve() const { return reason == IslandSolveRejectReason::None; }

/// Why one island sleep preflight would skip solve (B4.4 deepen pass).
enum class IslandSleepRejectReason : u8 {
    None = 0,
    EmptyIsland,
    AllSleeping,
    OutOfRangeIsland,
    NoSolveableIslands,
};

/// Human-readable label for island sleep reject reasons (logging / tests).
const char* islandSleepRejectReasonName(IslandSleepRejectReason reason);

/// Diagnose why one island sleep preflight would skip solve; vacuously succeeds when solve may proceed.
IslandSleepRejectReason islandSleepRejectReason(const ContactIslandGraph::Island& island,
                                                const RigidBodySoA& bodies);

/// Diagnose why one island sleep preflight would skip solve.

/// Diagnose why graph-level island sleep preflight would skip all solves.
IslandSleepRejectReason islandSleepGraphRejectReason(const ContactIslandGraph& graph,

/// Returns true when `islandSleepRejectReason` matches `expected` (B4.4 deepen pass).
bool islandSleepRejectsForReason(const ContactIslandGraph::Island& island,
                                 const RigidBodySoA& bodies,
                                 IslandSleepRejectReason expected);

/// Per-island sleep state for solve early-out stubs.
struct IslandSleepPreflight {
    IslandSleepRejectReason reason = IslandSleepRejectReason::None;
        return !skipped && reason == IslandConstraintSolveRejectReason::None && refs.can_solve() &&

/// Why per-island sleep solve would early-out (B4.5 deepen follow-up pass).
enum class IslandSleepSolveRejectReason : u8 {

/// Human-readable label for island sleep-solve reject reasons (B4.5 deepen follow-up pass).
const char* island_sleep_solve_reject_reason_name(IslandSleepSolveRejectReason reason);

/// Diagnose why island sleep solve would skip; vacuously succeeds when solve may proceed (B4.5 deepen follow-up pass).
IslandSleepSolveRejectReason island_sleep_solve_reject_reason(const ContactIslandGraph::Island& island,

/// Returns true when `island_sleep_solve_reject_reason` matches `expected` (B4.5 deepen follow-up pass).
bool island_sleep_solve_rejects_for_reason(const ContactIslandGraph::Island& island,
                                           IslandSleepSolveRejectReason expected);

/// Why island sleep solve would not early-out (B4.4 deepen follow-up pass).
    HasActiveDynamics,

/// Human-readable label for island sleep-solve reject reasons (logging / tests).

/// Diagnose why all-sleeping solve skip is unavailable; vacuously succeeds when skip may proceed.

/// Returns true when `island_sleep_solve_reject_reason` matches `expected`.

    IslandSleepSolveRejectReason reason = IslandSleepSolveRejectReason::None;
/// Why one island wake preflight would skip sleeper activation (B4.4 deepen pass).
enum class IslandWakeRejectReason : u8 {
    None = 0,
    EmptyIsland,
    OutOfRangeIsland,
    NoWakeTarget,
    NoWakeableIslands,
};

/// Human-readable label for island wake reject reasons (logging / tests).
const char* islandWakeRejectReasonName(IslandWakeRejectReason reason);

/// Diagnose why one island wake preflight would skip sleeper activation.
IslandWakeRejectReason islandWakeRejectReason(const ContactIslandGraph::Island& island,
                                              const RigidBodySoA& bodies);

/// Diagnose why graph-level island wake preflight would skip batch wake.
IslandWakeRejectReason islandWakeGraphRejectReason(const ContactIslandGraph& graph,

/// Returns true when `islandWakeRejectReason` matches `expected` (B4.4 deepen pass).
bool islandWakeRejectsForReason(const ContactIslandGraph::Island& island,
                                const RigidBodySoA& bodies,
                                IslandWakeRejectReason expected);

    u32 bodyCount = 0;
    u32 outOfRangeBodyCount = 0;
    u32 sleepingCount = 0;
    u32 staticOrKinematicCount = 0;
    u32 activeDynamicCount = 0;
    IslandSleepRejectReason reason = IslandSleepRejectReason::None;
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
        return !skipped && (reason == IslandSleepSolveRejectReason::AllSleeping || allSleeping);
    IslandSleepRejectReason reason = IslandSleepRejectReason::None;
    bool skipped = false;

    bool should_wake_sleepers() const {
        return !skipped && reason == IslandWakeRejectReason::None && hasMixedSleepState &&
               activeDynamicCount > 0u;
        return reason == IslandWakeRejectReason::None && !skipped && hasMixedSleepState && activeDynamicCount > 0u;
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
/// Non-mutating sleep-solve skip predicate — mirrors `can_skip_solve` (B4.4 deepen follow-up pass).
bool can_skip_island_sleep_solve_for_reason(const ContactIslandGraph::Island& island,

/// Non-mutating sleep-solve predicate — inverse of `can_skip_solve` (B4.4 deepen follow-up pass).
bool should_run_island_sleep_solve(const ContactIslandGraph::Island& island, const RigidBodySoA& bodies);

/// Why island wake preflight would skip sleeper activation (B4.4 deepen follow-up pass).
        return !skipped && reason == IslandSleepSolveRejectReason::None && allSleeping;

/// Why island sleeper wake would early-out (B4.4 deepen follow-up pass).
    bool emptyIsland = false;

    bool can_skip_solve() const { return !skipped && reason == IslandSleepRejectReason::AllSleeping; }

/// Why one island wake preflight would skip activation (B4.4 deepen pass).
enum class IslandWakeRejectReason : u8 {
    None = 0,
    EmptyIsland,
    NoMixedSleepState,
};

/// Human-readable label for island wake reject reasons (B4.4 deepen follow-up pass).
const char* island_wake_reject_reason_name(IslandWakeRejectReason reason);

/// Diagnose why island wake preflight would skip; vacuously succeeds when wake may proceed (B4.4 deepen follow-up pass).
IslandWakeRejectReason island_wake_reject_reason(const ContactIslandGraph::Island& island,

/// Returns true when `island_wake_reject_reason` matches `expected` (B4.4 deepen follow-up pass).
bool island_wake_rejects_for_reason(const ContactIslandGraph::Island& island,
                                    IslandWakeRejectReason expected);
    NoActiveDynamics,

/// Human-readable label for island wake reject reasons (logging / tests).

/// Diagnose why island sleeper wake would skip; vacuously succeeds when wake may proceed.

/// Returns true when `island_wake_reject_reason` matches `expected`.

/// Unified per-job dispatch preflight (dt + refs + bodies + sleep guards).
struct IslandDispatchJobPreflight {
    IslandSolveJobPreflight job{};
    IslandConstraintSolvePreflight constraint{};
    IslandSleepPreflight sleep{};
    IslandDispatchRejectReason reason = IslandDispatchRejectReason::None;
    bool skipped = false;

    bool can_dispatch() const { return reason == IslandDispatchRejectReason::None && !skipped; }
const char* islandWakeRejectReasonName(IslandWakeRejectReason reason);

/// Diagnose why one island wake preflight would skip activation; vacuously succeeds when wake may proceed.
IslandWakeRejectReason islandWakeRejectReason(const ContactIslandGraph::Island& island,

/// Returns true when `islandWakeRejectReason` matches `expected` (B4.4 deepen pass).
bool islandWakeRejectsForReason(const ContactIslandGraph::Island& island,

/// Per-island wake hint when active dynamics neighbor sleeping bodies.
struct IslandWakePreflight {
    IslandWakeRejectReason reason = IslandWakeRejectReason::None;
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
        return !skipped && reason == IslandSleepSolveRejectReason::AllSleeping && allSleeping;

/// Why per-island wake would early-out (B4.5 deepen follow-up pass).

/// Human-readable label for island wake reject reasons (B4.5 deepen follow-up pass).

/// Diagnose why island wake would skip; vacuously succeeds when wake may proceed (B4.5 deepen follow-up pass).

/// Returns true when `island_wake_reject_reason` matches `expected` (B4.5 deepen follow-up pass).
/// Combined constraint, body participation, and sleep preflight for one island solve pass.
struct IslandSolvePassPreflight {
    bool hasMixedSleepState = false;

    bool can_solve() const {
        return !skipped && constraint.can_solve() && !sleep.can_skip_solve();
        return reason == IslandSleepRejectReason::AllSleeping && !skipped && allSleeping;

/// Why one island wake preflight would early-out (B4.4 deepen follow-up pass).
    OutOfRangeIndex,
    NoActiveDynamic,


    u32 outOfRangeBodyCount = 0;

    bool should_wake_sleepers() const {
        return !skipped && hasMixedSleepState && activeDynamicCount > 0u;

/// Wake + constraint-solve pipeline preflight for one island (B4.4 deepen follow-up).
struct IslandSolvePipelinePreflight {
    IslandWakePreflight wake{};
    IslandConstraintSolvePreflight solve{};

    bool can_solve() const { return !skipped && solve.can_solve() && !sleep.can_skip_solve(); }
    bool should_wake_first() const { return !skipped && wake.should_wake_sleepers(); }

/// Why island sleeper wake would reject (B4.4 deepen follow-up pass).
    bool should_wake_sleepers() const { return reason == IslandWakeRejectReason::None && hasMixedSleepState; }
        return !skipped && reason == IslandWakeRejectReason::None && hasMixedSleepState &&
        return reason == IslandWakeRejectReason::None && !skipped && hasMixedSleepState &&
               activeDynamicCount > 0u;
        return reason == IslandWakeRejectReason::None && hasMixedSleepState && activeDynamicCount > 0u;
    }
    bool should_wake_sleepers() const { return !skipped && reason == IslandWakeRejectReason::None; }

/// Combined dispatch preflight with constraint-solve and sleep guards (B4.5 deepen follow-up pass).
struct IslandDispatchDeepenPreflight {
    IslandDispatchPreflight dispatch{};
    IslandConstraintSolvePreflight constraintSolve{};

    bool can_dispatch() const {
        return !skipped && dispatch.can_dispatch() && constraintSolve.can_solve() && !sleep.can_skip_solve();


/// Diagnose why wake would reject one island; `None` means wake may proceed.

IslandWakeRejectReason island_wake_reject_reason_by_index(const ContactIslandGraph& graph,


/// Read-only wake diagnostics with reject reason — no mutation (B4.4 deepen follow-up).
struct IslandWakeDeepenPreflight {

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

    bool can_solve_after_wake() const {
        return !skipped && !sleep.can_skip_solve();



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
/// Per-island wake outcome (skip vs activate) for parallel batch stubs.

/// Aggregate solveable counts for graph-level batch guards (refs + bodies + sleep).
struct IslandSolveableStats {
    u32 totalIslands = 0;
    u32 solveableCount = 0;
    u32 allSleepingCount = 0;
    u32 noMovableBodiesCount = 0;
    u32 staleRefsCount = 0;
    u32 emptyCount = 0;

/// Graph-level solveable preflight combining dispatch, constraint, and sleep guards.
struct IslandSolveableGraphPreflight {
    IslandSolveableStats stats{};
    bool invalidDt = false;

    bool can_dispatch() const { return !skipped && !invalidDt && stats.solveableCount > 0u; }



/// Combined sleep + wake + constraint-solve preflight for one island solve pipeline.

    bool can_skip_sleeping_solve() const { return !skipped && sleep.can_skip_solve(); }
    bool should_wake_sleepers() const { return !skipped && wake.should_wake_sleepers(); }
    bool can_constraint_solve() const { return !skipped && constraintSolve.can_solve(); }
        return !skipped && !sleep.can_skip_solve() && constraintSolve.can_solve();
    bool should_wake_sleepers() const { return reason == IslandWakeRejectReason::None; }

/// Batch wake + dispatch summary for parallel solve pipeline stubs.
struct IslandBatchWakeDispatchResult {
    u32 solvedCount = 0;
    u32 dispatchableCount = 0;

    bool any_solved() const { return solvedCount > 0u; }







/// Non-mutating wake skip predicate — inverse of `should_wake_sleepers` (B4.4 deepen follow-up pass).
bool can_skip_island_wake_for_reason(const ContactIslandGraph::Island& island, const RigidBodySoA& bodies);

/// Non-mutating wake predicate — mirrors `should_wake_sleepers` (B4.4 deepen follow-up pass).
/// Diagnose why per-island wake preflight would skip activation.
IslandWakeRejectReason islandWakeRejectReason(const IslandWakePreflight& preflight);

/// Aggregate sleep counts for graph-level batch guards.
struct IslandSleepGraphStats {
    u32 mixedSleepCount = 0;
    u32 fullyActiveCount = 0;

/// Why graph-level island sleep preflight would skip batch solve (B4.4 deepen pass).
enum class IslandSleepGraphRejectReason : u8 {
    None = 0,
    NoSolveableIslands,
};

/// Human-readable label for island sleep graph reject reasons (logging / tests).
const char* islandSleepGraphRejectReasonName(IslandSleepGraphRejectReason reason);

/// Diagnose why graph-level sleep preflight would skip batch solve; vacuously succeeds when solve may proceed.
IslandSleepGraphRejectReason islandSleepGraphRejectReason(const ContactIslandGraph& graph,
                                                          const RigidBodySoA& bodies);

/// Returns true when `islandSleepGraphRejectReason` matches `expected` (B4.4 deepen pass).
bool islandSleepGraphRejectsForReason(const ContactIslandGraph& graph,
                                      const RigidBodySoA& bodies,
                                      IslandSleepGraphRejectReason expected);
/// Why graph-level sleep solve would early-out (B4.4 deepen follow-up pass).
/// Why graph-level island sleep preflight would early-out (B4.4 deepen follow-up pass).
enum class IslandSleepGraphRejectReason : u8 {
    None = 0,
    NoSolveableIslands,
};

/// Human-readable label for island sleep-graph reject reasons (logging / tests).
const char* island_sleep_graph_reject_reason_name(IslandSleepGraphRejectReason reason);

/// Diagnose why graph-level sleep solve would skip.
IslandSleepGraphRejectReason island_sleep_graph_reject_reason(const ContactIslandGraph& graph,

/// Returns true when `island_sleep_graph_reject_reason` matches `expected`.
bool island_sleep_graph_rejects_for_reason(const ContactIslandGraph& graph,

/// Human-readable label for graph-level island sleep reject reasons (B4.4 deepen follow-up pass).

/// Graph-level sleep preflight for selective per-island solve skipping.
struct IslandSleepGraphPreflight {
    IslandSleepGraphRejectReason reason = IslandSleepGraphRejectReason::None;
    IslandSleepRejectReason reason = IslandSleepRejectReason::None;
    IslandSleepGraphStats stats{};
    IslandSleepGraphRejectReason reason = IslandSleepGraphRejectReason::None;
    IslandSleepRejectReason reason = IslandSleepRejectReason::None;
    bool skipped = false;

    bool has_solveable_islands() const {
        return !skipped && (stats.fullyActiveCount > 0u || stats.mixedSleepCount > 0u);
    bool has_solveable_islands() const { return reason == IslandSleepGraphRejectReason::None; }
    bool has_solveable_islands() const { return !skipped && reason == IslandSleepGraphRejectReason::None; }
        return !skipped && reason == IslandSleepGraphRejectReason::None &&
        return reason == IslandSleepGraphRejectReason::None && !skipped &&
               (stats.fullyActiveCount > 0u || stats.mixedSleepCount > 0u);
    }
};

/// Combined dispatch + sleep preflight for selective per-island solve skipping.
struct IslandDispatchSleepPreflight {
    IslandDispatchPreflight dispatch{};
    IslandSleepGraphPreflight sleep{};
    bool skipped = false;

    bool can_dispatch() const {
        return !skipped && dispatch.can_dispatch() && sleep.has_solveable_islands();
    }
    bool noSolveableIslands = false;


/// Combined constraint-solve + sleep preflight for one island dispatch pass.
struct IslandSolveDispatchPreflight {
    IslandConstraintSolvePreflight constraint{};
    IslandSleepPreflight sleep{};

        return !skipped && constraint.can_solve() && !sleep.can_skip_solve();
    IslandSleepRejectReason reason = IslandSleepRejectReason::None;

    bool has_solveable_islands() const { return reason == IslandSleepRejectReason::None; }
};

/// Graph-level combined dispatch + sleep preflight for batch solve guards.
struct IslandSleepSolveDispatchPreflight {


/// Aggregate solveable-island counts for sleep-aware batch guards.
struct IslandSolveableStats {
    u32 totalIslands = 0;
    u32 solveableCount = 0;
    u32 allSleepingCount = 0;
    u32 emptyCount = 0;
    u32 blockedCount = 0;

/// Combined constraint, body, and sleep preflight for one island solve pass.
struct IslandSolvePassPreflight {
    IslandSleepPreflight sleepState{};
    IslandWakePreflight wakeState{};

    bool can_solve() const {
        return !skipped && constraint.can_solve() && !sleepState.can_skip_solve();

    bool should_wake_first() const { return !skipped && wakeState.should_wake_sleepers(); }

/// Per-island wake outcome (skip vs wake) for parallel batch stubs.
struct IslandWakeResult {
    bool woke = false;
    u32 islandIndex = ContactIslandGraph::invalidIsland;

/// Graph-level nonsleeping dispatch preflight (solve sizing + sleep batch guards).
struct IslandNonsleepingDispatchPreflight {
    IslandSleepGraphPreflight sleepGraph{};

        return !skipped && dispatch.can_dispatch() && sleepGraph.has_solveable_islands();
/// Diagnose why graph-level sleep preflight would skip solve.
IslandSleepGraphRejectReason islandSleepGraphRejectReason(const IslandSleepGraphPreflight& preflight);

/// Aggregate wake counts for graph-level batch guards.
struct IslandWakeGraphStats {
    u32 wakeableCount = 0;

/// Why graph-level island wake preflight would skip batch activation (B4.4 deepen pass).
enum class IslandWakeGraphRejectReason : u8 {
    None = 0,
    NoWakeableIslands,
};

/// Human-readable label for island wake graph reject reasons (logging / tests).
const char* islandWakeGraphRejectReasonName(IslandWakeGraphRejectReason reason);

/// Diagnose why graph-level wake preflight would skip batch activation; vacuously succeeds when wake may proceed.
IslandWakeGraphRejectReason islandWakeGraphRejectReason(const ContactIslandGraph& graph,
                                                        const RigidBodySoA& bodies);

/// Returns true when `islandWakeGraphRejectReason` matches `expected` (B4.4 deepen pass).
bool islandWakeGraphRejectsForReason(const ContactIslandGraph& graph,
                                     const RigidBodySoA& bodies,
                                     IslandWakeGraphRejectReason expected);
/// Why graph-level wake would early-out (B4.4 deepen follow-up pass).
/// Why graph-level island wake preflight would early-out (B4.4 deepen follow-up pass).
enum class IslandWakeGraphRejectReason : u8 {
    None = 0,
    NoWakeableIslands,
};

/// Human-readable label for island wake-graph reject reasons (logging / tests).
const char* island_wake_graph_reject_reason_name(IslandWakeGraphRejectReason reason);

/// Diagnose why graph-level wake would skip.
IslandWakeGraphRejectReason island_wake_graph_reject_reason(const ContactIslandGraph& graph,

/// Returns true when `island_wake_graph_reject_reason` matches `expected`.
bool island_wake_graph_rejects_for_reason(const ContactIslandGraph& graph,

/// Human-readable label for graph-level island wake reject reasons (B4.4 deepen follow-up pass).

/// Graph-level wake preflight for selective per-island sleeper activation.
struct IslandWakeGraphPreflight {
    IslandWakeGraphRejectReason reason = IslandWakeGraphRejectReason::None;
    IslandWakeRejectReason reason = IslandWakeRejectReason::None;
    IslandWakeGraphStats stats{};
    IslandWakeGraphRejectReason reason = IslandWakeGraphRejectReason::None;
    IslandWakeRejectReason reason = IslandWakeRejectReason::None;
    bool skipped = false;

    bool can_wake() const { return !skipped && stats.wakeableCount > 0u; }

/// Per-island wake outcome (skip vs activate) for guarded entry points.
struct IslandWakeResult {
    bool woke = false;
    bool skipped = false;
    u32 islandIndex = ContactIslandGraph::invalidIsland;
    u32 sleepersWoken = 0;
    bool can_wake() const { return reason == IslandWakeGraphRejectReason::None; }
    bool noWakeableIslands = false;

    bool can_wake() const { return !skipped && reason == IslandWakeGraphRejectReason::None; }
    IslandWakeRejectReason reason = IslandWakeRejectReason::None;

    bool can_wake() const { return reason == IslandWakeRejectReason::None; }
    bool can_wake() const { return reason == IslandWakeGraphRejectReason::None && !skipped && stats.wakeableCount > 0u; }
    bool can_wake() const {
        return !skipped && reason == IslandWakeGraphRejectReason::None && stats.wakeableCount > 0u;
    }
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


/// Combined dispatch + sleep graph preflight for batch solve guards (B4.4 deepen).
struct IslandSleepDispatchPreflight {


/// Batch wake-then-solve summary for parallel dispatch stubs (B4.4 deepen).
struct IslandBatchSleepDispatchResult {
    u32 solveableCount = 0;
    u32 bodiesWoken = 0;


/// Graph-level dispatch preflight combining wake, sleep, and constraint-solve guards.
struct IslandDispatchSolveablePreflight {
    IslandConstraintSolveGraphPreflight constraintSolve{};

        return !skipped && dispatch.can_dispatch() && constraintSolve.can_dispatch();

/// Batch wake-then-solve summary for parallel iteration stubs.
struct IslandBatchDispatchSolveableResult {


/// Per-island wake outcome for batch sleeper activation stubs.
    u32 wokeBodyCount = 0;

/// Batch wake summary for graph-level sleeper activation stubs.
    u32 wokeIslandCount = 0;

    bool any_woke() const { return wokeIslandCount > 0u; }

/// Combined dispatch + sleep preflight for selective per-island solve skipping.


/// Combined wake-then-solve preflight for one island (B4.4 deepen follow-up pass).
struct IslandWakeThenSolvePreflight {
    IslandConstraintSolvePreflight solve{};

    bool can_wake_then_solve() const {
        return !skipped && sleep.can_skip_solve() == false && solve.can_solve();

/// Batch solve summary excluding all-sleeping islands (B4.4 deepen follow-up pass).
struct IslandNonsleepingDispatchResult {


/// Combined sleep/wake + constraint-solve preflight for guarded island dispatch.
struct IslandSleepWakeDispatchPreflight {
    IslandSleepPreflight islandSleep{};
    IslandWakePreflight islandWake{};
    bool invalidDt = false;

    bool can_wake() const { return !skipped && islandWake.should_wake_sleepers(); }

        return !skipped && !invalidDt && !islandSleep.can_skip_solve() && constraintSolve.can_solve();

/// Aggregate active-solve counts for graph-level sleep/wake dispatch guards.
struct IslandActiveSolveStats {
    u32 totalIslands = 0;
    u32 activeSolveCount = 0;
    u32 allSleepingCount = 0;
    u32 noMovableCount = 0;
    u32 emptyCount = 0;

/// Graph-level active-solve preflight for sleep/wake-aware batch dispatch.
struct IslandActiveSolveGraphPreflight {
    IslandActiveSolveStats stats{};

    bool can_dispatch() const { return !skipped && !invalidDt && stats.activeSolveCount > 0u; }

/// Batch sleep/wake dispatch summary for parallel iteration stubs.
struct IslandSleepWakeBatchDispatchResult {







/// Combined per-island solve dispatch preflight (constraint refs, bodies, sleep).
struct IslandSolveDispatchPreflight {

        return !skipped && constraint.can_solve() && !sleep.can_skip_solve();


/// Guarded dispatch outcome including optional sleeper wake.
struct IslandGuardedDispatchResult {
    bool solved = false;
    bool wokeSleepers = false;

/// Batch guarded dispatch summary with wake counts.
struct IslandBatchGuardedDispatchResult {




/// Per-island dispatch with wake preflight (dt + mixed-sleep guards).
struct IslandDispatchWithWakePreflight {

    bool can_dispatch() const { return !skipped && dispatch.can_dispatch(); }





/// Combined wake + constraint-solve preflight for one island dispatch pass.
struct IslandWakeAndSolvePreflight {

    bool can_dispatch() const { return !skipped && solve.can_solve(); }
    bool should_wake_first() const { return !skipped && wake.should_wake_sleepers(); }

/// Aggregate solveable counts for graph-level constraint-solve batch guards.
struct IslandSolveableStats {
    u32 noMovableBodiesCount = 0;
    u32 staleRefsCount = 0;

/// Graph-level constraint-solve preflight for selective per-island dispatch.
struct IslandSolveableGraphPreflight {
    IslandSolveableStats stats{};
/// Aggregate constraint-solve counts for graph-level batch guards.
struct IslandConstraintSolveGraphStats {
    u32 blockedByRefsCount = 0;
    u32 blockedByBodiesCount = 0;

struct IslandConstraintSolveGraphPreflight {
    IslandConstraintSolveGraphStats stats{};

    bool has_solveable_islands() const { return !skipped && stats.solveableCount > 0u; }


    bool can_wake() const { return !skipped && wake.should_wake_sleepers(); }
    bool can_solve() const { return !skipped && solve.can_solve(); }
    bool should_wake_before_solve() const { return can_wake() && can_solve(); }



/// Per-island sleep + wake + constraint-solve preflight for wake-then-solve stubs.
struct IslandSleepWakeSolvePreflight {

    bool can_skip_entirely() const { return !skipped && sleep.can_skip_solve(); }

    bool needs_wake() const { return !skipped && wake.should_wake_sleepers(); }

    bool can_solve() const { return !skipped && constraint.can_solve(); }

/// Aggregate counts for graph-level wake-then-solve batch guards.
struct IslandSleepWakeSolveGraphStats {

/// Graph-level sleep + wake + constraint-solve preflight for batch dispatch stubs.
struct IslandSleepWakeSolveGraphPreflight {
    IslandSleepWakeSolveGraphStats stats{};

    bool can_dispatch() const { return !skipped && stats.solveableCount > 0u; }

struct IslandSleepWakeSolveBatchResult {


/// Combined dispatch + sleep preflight for sleep-aware island solve batching.

/// Combined sleep + dispatch preflight for graph-level solve batching.

        return !skipped && !dispatch.invalidDt && dispatch.solve.can_dispatch() && sleep.has_solveable_islands();

    u32 sleeperCount = 0;



/// Combined wake + dispatch preflight for solve-after-wake batch stubs.
struct IslandDispatchAfterWakePreflight {

    bool needs_wake() const { return wake.can_wake(); }

/// Pre-solve pipeline preflight: wake hint + sleep + constraint readiness for one island.
struct IslandPreSolvePreflight {

    bool needs_wake() const { return wake.should_wake_sleepers(); }

    bool can_pre_solve() const {
        return !skipped && !sleep.can_skip_solve() && solve.can_solve();

/// Aggregate pre-solve counts for graph-level batch guards.
struct IslandPreSolveGraphStats {
    u32 preSolveableCount = 0;

/// Graph-level pre-solve pipeline preflight.
struct IslandPreSolveGraphPreflight {
    IslandPreSolveGraphStats stats{};

    bool can_pre_solve() const { return !skipped && stats.preSolveableCount > 0u; }

/// Per-job composed solve preflight (dispatch + constraint refs/bodies + sleep guards).
struct IslandJobSolvePreflight {
    IslandSolveJobPreflight job{};

        return !skipped && job.can_dispatch() && constraint.can_solve() && !sleep.can_skip_solve();

/// Aggregate solveable vs skipped island counts for graph-level batch guards.
struct IslandGraphSolveStats {
    u32 skippedSleepCount = 0;
    u32 skippedConstraintCount = 0;
    u32 skippedDispatchCount = 0;

/// Graph-level composed solve preflight (dispatch + sleep + constraint participation).
struct IslandGraphSolvePreflight {
    IslandGraphSolveStats solveStats{};

    bool can_solve() const { return !skipped && solveStats.solveableCount > 0u; }

/// Batch solve summary for composed guarded dispatch stubs.
struct IslandBatchSolveResult {
/// Batch dispatch summary that skips all-sleeping islands.
struct IslandBatchSleepSkipDispatchResult {


/// Combined constraint-ref, body participation, and sleep preflight for one island solve pass.
struct IslandFullSolvePreflight {
    IslandConstraintSolvePreflight constraints{};

        return !skipped && !invalidDt && constraints.can_solve() && !sleep.can_skip_solve();

/// Batch constraint-solve summary for parallel iteration stubs.
struct IslandBatchConstraintSolveResult {




/// Why per-island sleep solve early-out would not apply (B4.4 deepen follow-up pass).
enum class IslandSleepRejectReason : u8 {
    None = 0,
    EmptyIsland,
    OutOfRangeIndex,
    HasActiveDynamics,

/// Human-readable label for island-sleep reject reasons (logging / tests).
const char* islandSleepRejectReasonName(IslandSleepRejectReason reason);

/// Diagnose why sleep preflight cannot skip solve; `None` when all dynamics are sleeping.
IslandSleepRejectReason islandSleepRejectReason(const ContactIslandGraph::Island& island,
                                                const RigidBodySoA& bodies);

/// Returns true when `islandSleepRejectReason` matches `expected` (B4.4 deepen follow-up pass).
bool islandSleepRejectsForReason(const ContactIslandGraph::Island& island,
                                 const RigidBodySoA& bodies,
                                 IslandSleepRejectReason expected);

/// Read-only sleep-solve diagnostics with reject reason — no mutation (B4.4 deepen follow-up).
struct IslandSleepRejectPreflight {
    IslandSleepRejectReason reason = IslandSleepRejectReason::None;
    bool emptyIsland = false;
    bool hasActiveDynamics = false;

    bool can_skip_solve() const { return reason == IslandSleepRejectReason::None && sleep.can_skip_solve(); }

IslandSleepRejectPreflight preflight_island_sleep_reject(const ContactIslandGraph::Island& island,

IslandSleepRejectPreflight preflight_island_sleep_reject_by_index(const ContactIslandGraph& graph,
                                                                  u32 islandIndex,

/// Why per-island wake would reject (B4.4 deepen follow-up pass).
enum class IslandWakeRejectReason : u8 {
    NoMixedSleepState,
    NoActiveDynamic,

/// Human-readable label for island-wake reject reasons (logging / tests).
const char* islandWakeRejectReasonName(IslandWakeRejectReason reason);

/// Diagnose why wake would skip; vacuously succeeds when wake may proceed.
IslandWakeRejectReason islandWakeRejectReason(const ContactIslandGraph::Island& island,

/// Returns true when `islandWakeRejectReason` matches `expected` (B4.4 deepen follow-up pass).
bool islandWakeRejectsForReason(const ContactIslandGraph::Island& island,
                                IslandWakeRejectReason expected);

/// Read-only wake diagnostics with reject reason — no mutation (B4.4 deepen follow-up).
struct IslandWakeRejectPreflight {
    bool noMixedSleepState = false;
    bool noActiveDynamic = false;

    bool should_wake_sleepers() const {
        return reason == IslandWakeRejectReason::None && wake.should_wake_sleepers();

IslandWakeRejectPreflight preflight_island_wake_reject(const ContactIslandGraph::Island& island,

IslandWakeRejectPreflight preflight_island_wake_reject_by_index(const ContactIslandGraph& graph,

/// Why graph-level sleep batching would reject solve dispatch (B4.4 deepen follow-up pass).
enum class IslandSleepGraphRejectReason : u8 {
    AllSleepingOrEmpty,
/// Why island solve dispatch would early-out (B4.4 deepen follow-up pass).
enum class IslandDispatchRejectReason : u8 {
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
    bool skipped = false;

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
    u32 constraintCount = 0;

    bool can_dispatch() const { return reason == IslandSolveJobRejectReason::None; }


IslandSolveJobRejectPreflight preflightIslandSolveJobReject(const IslandSolveJob& job, f32 dt);

bool canSkipIslandSolveJob(const IslandSolveJob& job, f32 dt);

bool shouldRunIslandSolveJob(const IslandSolveJob& job, f32 dt);

/// Why one island constraint solve would early-out (B4.4 deepen follow-up pass).
enum class IslandConstraintSolveRejectReason : u8 {
    NoInRangeRefs,
    NoMovableBodies,

const char* islandConstraintSolveRejectReasonName(IslandConstraintSolveRejectReason reason);

IslandConstraintSolveRejectReason islandConstraintSolveRejectReason(
    const ContactIslandGraph::Island& island,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints);

bool islandConstraintSolveRejectsForReason(
const char* island_dispatch_reject_reason_name(IslandDispatchRejectReason reason);

IslandDispatchRejectReason island_dispatch_reject_reason(const ContactIslandGraph& graph, f32 dt);

bool island_dispatch_rejects_for_reason(const ContactIslandGraph& graph,

    bool invalidDt = false;

                                        f32 dt,
                                        IslandDispatchRejectReason expected);

struct IslandDispatchRejectPreflight {
    IslandDispatchRejectReason reason = IslandDispatchRejectReason::None;
    IslandSolvePreflight solve{};
    bool invalidDt = false;
    bool skipped = false;

    bool can_dispatch() const { return reason == IslandDispatchRejectReason::None; }
};

IslandDispatchRejectPreflight preflight_island_dispatch_reject(const ContactIslandGraph& graph, f32 dt);

bool can_skip_island_dispatch(const ContactIslandGraph& graph, f32 dt);

bool should_run_island_dispatch(const ContactIslandGraph& graph, f32 dt);

    None = 0,
    InvalidDt,
    NonFiniteDt,
/// Why one island solve job would reject dispatch (B4.4 deepen follow-up pass).
enum class IslandSolveJobRejectReason : u8 {
    EmptyJob,
    NullIsland,
    ZeroConstraints,
};

const char* island_solve_job_reject_reason_name(IslandSolveJobRejectReason reason);

IslandSolveJobRejectReason island_solve_job_reject_reason(const IslandSolveJob& job, f32 dt);

bool island_solve_job_rejects_for_reason(const IslandSolveJob& job,


                                         f32 dt,
                                         IslandSolveJobRejectReason expected);

struct IslandSolveJobRejectPreflight {
    IslandSolveJobRejectReason reason = IslandSolveJobRejectReason::None;
    bool invalidDt = false;
    bool skipped = false;
    u32 constraintCount = 0;

    bool can_dispatch() const { return reason == IslandSolveJobRejectReason::None; }
};

IslandSolveJobRejectPreflight preflight_island_solve_job_reject(const IslandSolveJob& job, f32 dt);

bool can_skip_island_solve_job(const IslandSolveJob& job, f32 dt);

bool should_run_island_solve_job(const IslandSolveJob& job, f32 dt);

    EmptyIsland,
/// Why one island constraint solve would early-out (B4.4 deepen follow-up pass).
enum class IslandConstraintSolveRejectReason : u8 {
    None = 0,
    NoInRangeRefs,
    NoMovableBodies,
};

const char* island_constraint_solve_reject_reason_name(IslandConstraintSolveRejectReason reason);

IslandConstraintSolveRejectReason island_constraint_solve_reject_reason(
    const RigidBodySoA& bodies,

bool island_constraint_solve_rejects_for_reason(
    const ContactIslandGraph::Island& island,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints);


    const std::vector<DistanceConstraint>& distanceConstraints,
    IslandConstraintSolveRejectReason expected);

struct IslandConstraintSolveRejectPreflight {
    IslandConstraintSolveRejectReason reason = IslandConstraintSolveRejectReason::None;
    IslandConstraintRefsPreflight refs{};
    IslandSolveBodiesPreflight bodies{};

    bool can_solve() const { return reason == IslandConstraintSolveRejectReason::None; }

IslandConstraintSolveRejectPreflight preflightIslandConstraintSolveReject(

bool canSkipIslandConstraintSolve(const ContactIslandGraph::Island& island,

bool shouldRunIslandConstraintSolve(const ContactIslandGraph::Island& island,

/// Why per-island sleep solve would early-out (B4.4 deepen follow-up pass).
enum class IslandSleepSolveRejectReason : u8 {
    AllSleeping,






const char* islandSleepSolveRejectReasonName(IslandSleepSolveRejectReason reason);

IslandSleepSolveRejectReason islandSleepSolveRejectReason(const ContactIslandGraph::Island& island,

bool islandSleepSolveRejectsForReason(const ContactIslandGraph::Island& island,

                                      IslandSleepSolveRejectReason expected);

struct IslandSleepSolveRejectPreflight {
    IslandSleepSolveRejectReason reason = IslandSleepSolveRejectReason::None;

    bool can_solve() const { return reason == IslandSleepSolveRejectReason::None; }

IslandSleepSolveRejectPreflight preflightIslandSleepSolveReject(const ContactIslandGraph::Island& island,



bool canSkipIslandSleepSolve(const ContactIslandGraph::Island& island, const RigidBodySoA& bodies);

bool shouldRunIslandSleepSolve(const ContactIslandGraph::Island& island, const RigidBodySoA& bodies);

/// Why per-island wake would early-out (B4.4 deepen follow-up pass).




    IslandWakeRejectReason reason = IslandWakeRejectReason::None;

    bool can_wake() const { return reason == IslandWakeRejectReason::None; }

IslandWakeRejectPreflight preflightIslandWakeReject(const ContactIslandGraph::Island& island,







bool canSkipIslandWake(const ContactIslandGraph::Island& island, const RigidBodySoA& bodies);

bool shouldRunIslandWake(const ContactIslandGraph::Island& island, const RigidBodySoA& bodies);

/// Why graph-level sleep batching would early-out (B4.4 deepen follow-up pass).
    AllIslandsSleepingOrEmpty,

const char* islandSleepGraphRejectReasonName(IslandSleepGraphRejectReason reason);

IslandSleepGraphRejectReason islandSleepGraphRejectReason(const ContactIslandGraph& graph,

bool islandSleepGraphRejectsForReason(const ContactIslandGraph& graph,


                                      IslandSleepGraphRejectReason expected);

struct IslandSleepGraphRejectPreflight {
    IslandSleepGraphRejectReason reason = IslandSleepGraphRejectReason::None;
    bool allSleepingOrEmpty = false;

    bool has_solveable_islands() const {
        return reason == IslandSleepGraphRejectReason::None && sleep.has_solveable_islands();

IslandSleepGraphRejectPreflight preflight_island_sleep_graph_reject(const ContactIslandGraph& graph,

bool canSkipIslandSleepSolveGraph(const ContactIslandGraph& graph, const RigidBodySoA& bodies);

bool shouldRunIslandSleepSolveGraph(const ContactIslandGraph& graph, const RigidBodySoA& bodies);

/// Why graph-level wake batching would reject (B4.4 deepen follow-up pass).
enum class IslandWakeGraphRejectReason : u8 {
    NoWakeableIslands,

    bool has_solveable_islands() const { return reason == IslandSleepGraphRejectReason::None; }

IslandSleepGraphRejectPreflight preflightIslandSleepGraphReject(const ContactIslandGraph& graph,

bool canSkipIslandSleepGraph(const ContactIslandGraph& graph, const RigidBodySoA& bodies);

bool shouldRunIslandSleepGraph(const ContactIslandGraph& graph, const RigidBodySoA& bodies);

/// Why graph-level wake batching would early-out (B4.4 deepen follow-up pass).

const char* islandWakeGraphRejectReasonName(IslandWakeGraphRejectReason reason);

IslandWakeGraphRejectReason islandWakeGraphRejectReason(const ContactIslandGraph& graph,

bool islandWakeGraphRejectsForReason(const ContactIslandGraph& graph,


                                     IslandWakeGraphRejectReason expected);

struct IslandWakeGraphRejectPreflight {
    IslandWakeGraphRejectReason reason = IslandWakeGraphRejectReason::None;

    bool can_wake() const { return reason == IslandWakeGraphRejectReason::None && wake.can_wake(); }

IslandWakeGraphRejectPreflight preflight_island_wake_graph_reject(const ContactIslandGraph& graph,

    bool can_wake() const { return reason == IslandWakeGraphRejectReason::None; }

IslandWakeGraphRejectPreflight preflightIslandWakeGraphReject(const ContactIslandGraph& graph,

bool canSkipIslandWakeGraph(const ContactIslandGraph& graph, const RigidBodySoA& bodies);

bool shouldRunIslandWakeGraph(const ContactIslandGraph& graph, const RigidBodySoA& bodies);
/// Why island graph build would early-out (B4.4 deepen follow-up).
enum class IslandBuildRejectReason : u8 {
    EmptyInputs,
    OutOfRangeContactBodies,
    OutOfRangeDistanceBodies,

/// Human-readable label for island build reject reasons (logging / tests).
const char* island_build_reject_reason_name(IslandBuildRejectReason reason);

/// Diagnose why island build would reject; vacuously succeeds when build may proceed.
IslandBuildRejectReason island_build_reject_reason(
    u32 bodyCount,

/// Returns true when `island_build_reject_reason` matches `expected` (B4.4 deepen follow-up).
bool island_build_rejects_for_reason(
    IslandBuildRejectReason expected);

/// Extended build preflight with reject reason (B4.4 deepen follow-up).
struct IslandBuildDeepenPreflight {
    IslandBuildRejectReason reason = IslandBuildRejectReason::None;
    IslandBuildPreflight base{};
    bool rejected = false;

    bool can_build() const { return !rejected; }

/// Populate extended island build preflight without mutating the graph (B4.4 deepen follow-up).
IslandBuildDeepenPreflight preflight_island_build_deepen(

/// Non-mutating build skip predicate for deepen preflight (B4.4 deepen follow-up).
bool should_skip_island_build_deepen(u32 bodyCount,

/// Why island constraint solve would early-out (B4.4 deepen follow-up).
    StaleConstraintRefs,

const char* island_constraint_solve_reject_reason_name(IslandConstraintSolveRejectReason reason);

IslandConstraintSolveRejectReason island_constraint_solve_reject_reason(

bool island_constraint_solve_rejects_for_reason(

struct IslandConstraintSolveDeepenPreflight {
    IslandConstraintSolvePreflight base{};

    bool can_solve() const { return !rejected; }

IslandConstraintSolveDeepenPreflight preflight_island_constraint_solve_deepen(

bool should_skip_island_constraint_solve_deepen(const ContactIslandGraph::Island& island,

/// Why island sleep solve would early-out (B4.4 deepen follow-up).
    OutOfRangeIsland,

const char* island_sleep_solve_reject_reason_name(IslandSleepSolveRejectReason reason);

IslandSleepSolveRejectReason island_sleep_solve_reject_reason(

bool island_sleep_solve_rejects_for_reason(const ContactIslandGraph::Island& island,

struct IslandSleepSolveDeepenPreflight {
    IslandSleepPreflight base{};


IslandSleepSolveDeepenPreflight preflight_island_sleep_solve_deepen(

bool should_skip_island_sleep_solve_deepen(const ContactIslandGraph::Island& island,

/// Why island wake would early-out (B4.4 deepen follow-up).
    NoWakeTarget,
    bool skipped = false;



};

IslandConstraintSolveRejectPreflight preflight_island_constraint_solve_reject(
    const ContactIslandGraph::Island& island,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints);

bool can_skip_island_constraint_solve(const ContactIslandGraph::Island& island,

bool should_run_island_constraint_solve(const ContactIslandGraph::Island& island,

    None = 0,
    EmptyIsland,

                                      const RigidBodySoA& bodies,
                                      const std::vector<narrowphase::ContactManifold>& contacts,
                                      const std::vector<DistanceConstraint>& distanceConstraints);


/// Why per-island sleep solve would early-out (B4.4 deepen follow-up pass).
enum class IslandSleepSolveRejectReason : u8 {
bool should_run_island_constraint_solve(const ContactIslandGraph::Island& island,
                                          const RigidBodySoA& bodies,
                                          const std::vector<narrowphase::ContactManifold>& contacts,
                                          const std::vector<DistanceConstraint>& distanceConstraints);

    None = 0,
    EmptyIsland,
    AllSleeping,
};

const char* island_sleep_solve_reject_reason_name(IslandSleepSolveRejectReason reason);

IslandSleepSolveRejectReason island_sleep_solve_reject_reason(const ContactIslandGraph::Island& island,
                                                              const RigidBodySoA& bodies);


    IslandSleepPreflight sleep{};


IslandSleepSolveRejectPreflight preflight_island_sleep_solve_reject(const ContactIslandGraph::Island& island,
bool island_sleep_solve_rejects_for_reason(const ContactIslandGraph::Island& island,
                                           const RigidBodySoA& bodies,
                                           IslandSleepSolveRejectReason expected);

struct IslandSleepSolveRejectPreflight {
    IslandSleepSolveRejectReason reason = IslandSleepSolveRejectReason::None;
    IslandSleepPreflight sleep{};
    bool skipped = false;

    bool can_solve() const { return reason == IslandSleepSolveRejectReason::None; }
};

IslandSleepSolveRejectPreflight preflight_island_sleep_solve_reject(const ContactIslandGraph::Island& island,
                                                                    const RigidBodySoA& bodies);

bool can_skip_island_sleep_solve(const ContactIslandGraph::Island& island, const RigidBodySoA& bodies);

bool should_run_island_sleep_solve(const ContactIslandGraph::Island& island, const RigidBodySoA& bodies);

enum class IslandWakeRejectReason : u8 {
    NoMixedSleepState,
    NoActiveDynamic,
/// Why per-island wake would early-out (B4.4 deepen follow-up pass).
    None = 0,
    EmptyIsland,
};

const char* island_wake_reject_reason_name(IslandWakeRejectReason reason);

IslandWakeRejectReason island_wake_reject_reason(const ContactIslandGraph::Island& island,

bool island_wake_rejects_for_reason(const ContactIslandGraph::Island& island,

struct IslandWakeDeepenPreflight {
    IslandWakePreflight base{};

    bool can_wake() const { return !rejected; }

IslandWakeDeepenPreflight preflight_island_wake_deepen(const ContactIslandGraph::Island& island,

bool should_skip_island_wake_deepen(const ContactIslandGraph::Island& island,

/// Combined wake + constraint-solve + dispatch preflight for one island (B4.4 deepen follow-up).
    IslandWakeDeepenPreflight wake{};
    IslandConstraintSolveDeepenPreflight solve{};
    IslandSleepSolveDeepenPreflight sleep{};

        return !skipped && dispatch.can_dispatch() && sleep.can_solve() && solve.can_solve();

IslandSolvePipelinePreflight preflight_island_solve_pipeline(const ContactIslandGraph& graph,
                                                             f32 dt);

/// Batch wake summary for graph-level activation guards (B4.4 deepen follow-up).


/// Batch wake with explicit skip/woke counts (B4.4 deepen follow-up).
IslandBatchWakeResult wake_all_island_sleepers_result(RigidBodySoA& bodies,
                                                      const ContactIslandGraph& graph);




/// Per-island pipeline solve preflight (job + sleep + constraint guards).
struct IslandPipelineSolvePreflight {

        return !skipped && job.can_dispatch() && !sleep.can_skip_solve() && constraint.can_solve();

/// Aggregate counts for pipeline dispatch sizing and selective solve skipping.
struct IslandPipelineDispatchStats {
    u32 pipelineDispatchableCount = 0;

/// Combined pipeline dispatch preflight (graph dispatch + per-island solve guards).
struct IslandPipelineDispatchPreflight {
    IslandPipelineDispatchStats stats{};

        return !skipped && dispatch.can_dispatch() && stats.pipelineDispatchableCount > 0u;

/// Per-island pipeline dispatch outcome (wake vs skip vs solve).
struct IslandPipelineDispatchResult {

/// Batch pipeline dispatch summary for wake-then-solve iteration stubs.

/// Diagnostic reason one island constraint solve would reject (B4.4 deepen follow-up).
enum class IslandSolveRejectReason : u8 {
    None,

/// Human-readable label for island solve reject reasons (B4.4 deepen follow-up).
const char* island_solve_reject_reason_name(IslandSolveRejectReason reason);

/// Returns the first reject reason for one island solve pass.
IslandSolveRejectReason island_solve_reject_reason(

/// Returns true when `island_solve_reject_reason` matches `expected` (B4.4 deepen follow-up).
bool island_solve_rejects_for_reason(
    IslandSolveRejectReason expected);

/// Extended constraint-solve preflight combining refs, bodies, and sleep guards.
struct IslandExtendedSolvePreflight {
    IslandSolveRejectReason reason = IslandSolveRejectReason::None;

    bool can_solve() const { return !skipped && reason == IslandSolveRejectReason::None; }

/// Populate extended island solve preflight without mutating bodies (B4.4 deepen follow-up).
IslandExtendedSolvePreflight preflight_island_extended_solve(

/// Early-out guard when extended island solve preflight rejects the pass.
bool should_skip_island_extended_solve(

/// Diagnostic reason island pipeline dispatch would reject (B4.4 deepen follow-up).
enum class IslandPipelineRejectReason : u8 {
    AllIslandsSleeping,

/// Human-readable label for island pipeline reject reasons (B4.4 deepen follow-up).
const char* island_pipeline_reject_reason_name(IslandPipelineRejectReason reason);

/// Returns the first reject reason for graph-level island pipeline dispatch.
IslandPipelineRejectReason island_pipeline_reject_reason(const ContactIslandGraph& graph,

/// Returns true when `island_pipeline_reject_reason` matches `expected` (B4.4 deepen follow-up).
bool island_pipeline_rejects_for_reason(const ContactIslandGraph& graph,
                                        IslandPipelineRejectReason expected);

/// Combined wake + dispatch + sleep preflight for one substep pipeline stub.
struct IslandPipelinePreflight {
    IslandPipelineRejectReason reason = IslandPipelineRejectReason::None;

    bool can_dispatch() const { return !skipped && reason == IslandPipelineRejectReason::None; }

/// Populate island pipeline dispatch preflight without mutating bodies (B4.4 deepen follow-up).
IslandPipelinePreflight preflight_island_pipeline_dispatch(const ContactIslandGraph& graph,

/// Early-out guard when island pipeline dispatch should not run.
bool should_skip_island_pipeline_dispatch(const ContactIslandGraph& graph,

/// Per-island pipeline dispatch outcome (wake vs solve vs skip).

struct IslandBatchPipelineDispatchResult {

/// Per-island solve pipeline preflight (job + constraint + sleep guards).

        return !skipped && job.can_dispatch() && constraintSolve.can_solve() && !sleep.can_skip_solve();

/// Aggregate counts for pipeline dispatch sizing and skip stubs.
    u32 sleepSkippedCount = 0;
    u32 constraintSkippedCount = 0;

/// Graph-level pipeline dispatch preflight (dispatch + sleep + wake guards).

    bool can_run() const { return !skipped && dispatch.can_dispatch() && sleep.has_solveable_islands(); }

/// Per-island pipeline dispatch outcome (wake + solve) for batch stubs.



/// Per-island wake outcome (skip vs activate) for pipeline batch stubs.

/// Diagnostic reason island batch dispatch would early-out (B4.4 deepen follow-up).

/// Human-readable label for diagnostics and test assertions (B4.4 deepen follow-up).
const char* island_dispatch_reject_reason_name(IslandDispatchRejectReason reason);

/// Diagnostic reason one island constraint solve would early-out (B4.4 deepen follow-up).


/// Combined pipeline preflight for build → wake → dispatch (B4.4 deepen follow-up).
    IslandBuildPreflight build{};
    IslandDispatchRejectReason rejectReason = IslandDispatchRejectReason::None;

    bool can_run() const { return !skipped && rejectReason == IslandDispatchRejectReason::None; }

/// Per-island wake outcome for batch stubs (B4.4 deepen follow-up).

/// Batch wake summary for pipeline dispatch stubs (B4.4 deepen follow-up).

    bool can_wake() const {
        return !skipped && reason == IslandWakeGraphRejectReason::None && stats.wakeableCount > 0u;

/// Why wake-then-dispatch pipeline would early-out (B4.4 deepen follow-up pass).
enum class IslandPipelineDispatchRejectReason : u8 {

/// Human-readable label for island pipeline dispatch reject reasons (logging / tests).
const char* island_pipeline_dispatch_reject_reason_name(IslandPipelineDispatchRejectReason reason);

/// Diagnose why wake-then-dispatch pipeline would skip.
IslandPipelineDispatchRejectReason island_pipeline_dispatch_reject_reason(const ContactIslandGraph& graph,

/// Returns true when `island_pipeline_dispatch_reject_reason` matches `expected`.
bool island_pipeline_dispatch_rejects_for_reason(const ContactIslandGraph& graph,
                                                 IslandPipelineDispatchRejectReason expected);

/// Combined wake, sleep, and dispatch preflight for one pipeline pass.
    IslandPipelineDispatchRejectReason reason = IslandPipelineDispatchRejectReason::None;

    bool can_dispatch() const { return reason == IslandPipelineDispatchRejectReason::None; }

/// Read-only wake-then-dispatch pipeline diagnostics — no mutation.
IslandPipelineDispatchPreflight preflight_island_pipeline_dispatch(const ContactIslandGraph& graph,

/// Non-mutating pipeline skip predicate — inverse of `can_dispatch`.

/// Non-mutating pipeline predicate — mirrors `preflight_island_pipeline_dispatch`.
bool should_run_island_pipeline_dispatch(const ContactIslandGraph& graph,

/// Wake sleepers then dispatch all islands only when pipeline preflight allows.



const char* islandPipelineDispatchRejectReasonName(IslandPipelineDispatchRejectReason reason);

IslandPipelineDispatchRejectReason islandPipelineDispatchRejectReason(const ContactIslandGraph& graph,

bool islandPipelineDispatchRejectsForReason(const ContactIslandGraph& graph,



    IslandWakeGraphRejectPreflight wake{};
    IslandSleepGraphRejectPreflight sleep{};
    IslandDispatchRejectPreflight dispatch{};


IslandPipelineDispatchPreflight preflightIslandPipelineDispatch(const ContactIslandGraph& graph,


bool canSkipIslandPipelineDispatch(const ContactIslandGraph& graph, const RigidBodySoA& bodies, f32 dt);

bool shouldRunIslandPipelineDispatch(const ContactIslandGraph& graph, const RigidBodySoA& bodies, f32 dt);

/// Wake sleepers then dispatch all islands only when pipeline preflight allows (B4.4 deepen follow-up pass).
                                                 const RigidBodySoA& bodies);



                                    const RigidBodySoA& bodies,
                                    IslandWakeRejectReason expected);

struct IslandWakeRejectPreflight {
    IslandWakeRejectReason reason = IslandWakeRejectReason::None;
    IslandWakePreflight wake{};
    bool skipped = false;

    bool can_wake() const { return reason == IslandWakeRejectReason::None; }
};

IslandWakeRejectPreflight preflight_island_wake_reject(const ContactIslandGraph::Island& island,
                                                       const RigidBodySoA& bodies);

bool can_skip_island_wake(const ContactIslandGraph::Island& island, const RigidBodySoA& bodies);

bool should_run_island_wake(const ContactIslandGraph::Island& island, const RigidBodySoA& bodies);

/// Why graph-level sleep batching would early-out (B4.4 deepen follow-up pass).
enum class IslandSleepGraphRejectReason : u8 {
    None = 0,
    AllIslandsSleepingOrEmpty,
};

const char* island_sleep_graph_reject_reason_name(IslandSleepGraphRejectReason reason);

IslandSleepGraphRejectReason island_sleep_graph_reject_reason(const ContactIslandGraph& graph,

bool island_sleep_graph_rejects_for_reason(const ContactIslandGraph& graph,
                                                              const RigidBodySoA& bodies);


                                           const RigidBodySoA& bodies,
                                           IslandSleepGraphRejectReason expected);

struct IslandSleepGraphRejectPreflight {
    IslandSleepGraphRejectReason reason = IslandSleepGraphRejectReason::None;
    IslandSleepGraphPreflight sleep{};

    bool has_solveable_islands() const { return reason == IslandSleepGraphRejectReason::None; }

IslandSleepGraphRejectPreflight preflight_island_sleep_graph_reject(const ContactIslandGraph& graph,
    bool skipped = false;

};



                                                                    const RigidBodySoA& bodies);

bool can_skip_island_sleep_graph(const ContactIslandGraph& graph, const RigidBodySoA& bodies);

bool should_run_island_sleep_graph(const ContactIslandGraph& graph, const RigidBodySoA& bodies);

/// Why graph-level wake batching would early-out (B4.4 deepen follow-up pass).
enum class IslandWakeGraphRejectReason : u8 {
    NoWakeableIslands,
    None = 0,
};

const char* island_wake_graph_reject_reason_name(IslandWakeGraphRejectReason reason);

IslandWakeGraphRejectReason island_wake_graph_reject_reason(const ContactIslandGraph& graph,

bool island_wake_graph_rejects_for_reason(const ContactIslandGraph& graph,
                                                            const RigidBodySoA& bodies);


                                          const RigidBodySoA& bodies,
                                          IslandWakeGraphRejectReason expected);

struct IslandWakeGraphRejectPreflight {
    IslandWakeGraphRejectReason reason = IslandWakeGraphRejectReason::None;
    IslandWakeGraphPreflight wake{};

    bool can_wake() const { return reason == IslandWakeGraphRejectReason::None; }

IslandWakeGraphRejectPreflight preflight_island_wake_graph_reject(const ContactIslandGraph& graph,
    bool skipped = false;

};



                                                                  const RigidBodySoA& bodies);

bool can_skip_island_wake_graph(const ContactIslandGraph& graph, const RigidBodySoA& bodies);

bool should_run_island_wake_graph(const ContactIslandGraph& graph, const RigidBodySoA& bodies);

    InvalidDt,
    NonFiniteDt,
    NoDispatchableIslands,



                                                 f32 dt,




bool can_skip_island_pipeline_dispatch(const ContactIslandGraph& graph,


/// Why wake-then-dispatch pipeline would early-out (B4.4 deepen follow-up pass).
enum class IslandPipelineDispatchRejectReason : u8 {
    None = 0,
    AllIslandsSleeping,
};

const char* island_pipeline_dispatch_reject_reason_name(IslandPipelineDispatchRejectReason reason);

IslandPipelineDispatchRejectReason island_pipeline_dispatch_reject_reason(const ContactIslandGraph& graph,
                                                                          const RigidBodySoA& bodies,
                                                                          f32 dt);

bool island_pipeline_dispatch_rejects_for_reason(const ContactIslandGraph& graph,
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

IslandPipelineDispatchPreflight preflight_island_pipeline_dispatch(const ContactIslandGraph& graph,


bool should_run_island_pipeline_dispatch(const ContactIslandGraph& graph,
};

                                                                   const RigidBodySoA& bodies,
                                                                   f32 dt);

bool can_skip_island_pipeline_dispatch(const ContactIslandGraph& graph,


/// Wake sleepers then dispatch all islands only when pipeline preflight allows.
IslandBatchDispatchResult dispatch_island_pipeline_guarded(
    RigidBodySoA& bodies,
    const ContactIslandGraph& graph,
    SolverWorkBuffers& workBuffers,
    f32 contactCompliance,
    const std::function<f32(const RigidBodySoA&, u32)>& invMassFn);
/// Diagnose why graph-level wake preflight would skip activation.
IslandWakeGraphRejectReason islandWakeGraphRejectReason(const IslandWakeGraphPreflight& preflight);
    const std::vector<DistanceConstraint>& distanceConstraints,
    f32 dt,

/// Batch guarded dispatch with explicit reject-reason preflight.
IslandBatchDispatchResult dispatch_all_islands_with_preflight(
    RigidBodySoA& bodies,
    const ContactIslandGraph& graph,
    SolverWorkBuffers& workBuffers,
    const std::vector<DistanceConstraint>& distanceConstraints,
    f32 dt,
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

/// Diagnose why island solve dispatch would skip for one extracted job.
IslandDispatchRejectReason island_dispatch_reject_reason(const IslandSolveJob& job, f32 dt);

/// Diagnose why island graph-level dispatch would skip.
IslandDispatchRejectReason island_dispatch_reject_reason(const ContactIslandGraph& graph, f32 dt);

/// Diagnose why one island constraint solve would skip.
IslandSolveRejectReason island_solve_reject_reason(const ContactIslandGraph::Island& island,
                                                   const RigidBodySoA& bodies,
                                                   const std::vector<narrowphase::ContactManifold>& contacts,
                                                   const std::vector<DistanceConstraint>& distanceConstraints,
                                                   f32 dt);

/// Diagnose why island sleep preflight would skip for one island.
IslandSleepRejectReason island_sleep_reject_reason(const ContactIslandGraph::Island& island,
                                                  const RigidBodySoA& bodies);

/// Diagnose why island wake preflight would skip for one island.
IslandWakeRejectReason island_wake_reject_reason(const ContactIslandGraph::Island& island,
                                                const RigidBodySoA& bodies);

/// Returns true when `island_dispatch_reject_reason` matches `expected` (B4.4 deepen follow-up pass).
bool island_dispatch_rejects_for_reason(const IslandSolveJob& job,
                                        f32 dt,
                                        IslandDispatchRejectReason expected);

/// Returns true when graph-level `island_dispatch_reject_reason` matches `expected`.
bool island_dispatch_rejects_for_reason(const ContactIslandGraph& graph,
                                        f32 dt,
                                        IslandDispatchRejectReason expected);

/// Returns true when `island_solve_reject_reason` matches `expected`.
bool island_solve_rejects_for_reason(const ContactIslandGraph::Island& island,
                                     const RigidBodySoA& bodies,
                                     const std::vector<narrowphase::ContactManifold>& contacts,
                                     const std::vector<DistanceConstraint>& distanceConstraints,
                                     f32 dt,
                                     IslandSolveRejectReason expected);

/// Returns true when `island_sleep_reject_reason` matches `expected`.
bool island_sleep_rejects_for_reason(const ContactIslandGraph::Island& island,
                                     const RigidBodySoA& bodies,
                                     IslandSleepRejectReason expected);

/// Returns true when `island_wake_reject_reason` matches `expected`.
bool island_wake_rejects_for_reason(const ContactIslandGraph::Island& island,
                                    const RigidBodySoA& bodies,
                                    IslandWakeRejectReason expected);

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

IslandDispatchRejectReason islandDispatchRejectReason(const IslandSolveJob& job, f32 dt);

IslandDispatchRejectReason islandDispatchRejectReason(
    const IslandSolveJob& job,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints,
    f32 dt);

bool islandDispatchRejectsForReason(const IslandSolveJob& job,
                                    f32 dt,
                                    IslandDispatchRejectReason expected);

bool islandDispatchRejectsForReason(const IslandSolveJob& job,
                                    const RigidBodySoA& bodies,
                                    const std::vector<narrowphase::ContactManifold>& contacts,
                                    const std::vector<DistanceConstraint>& distanceConstraints,
                                    f32 dt,
                                    IslandDispatchRejectReason expected);

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
/// Combined dispatch + sleep preflight; sets `skipped` when every constrained island is all-sleeping.
IslandDispatchSleepPreflight preflight_island_dispatch_sleep(const ContactIslandGraph& graph,

/// Early-out guard combining dispatch, timestep, and all-sleeping graph checks.
bool should_skip_island_dispatch_sleep(const ContactIslandGraph& graph,

/// Preflight a built island graph for out-of-range body and constraint references.
IslandBuiltGraphPreflight preflight_built_island_graph(const ContactIslandGraph& graph,
                                                       u32 bodyCount,
                                                       const std::vector<DistanceConstraint>& distanceConstraints);

/// Early-out guard when a built graph carries out-of-range references.
bool should_skip_built_island_graph(const ContactIslandGraph& graph,

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
/// Guarded dispatch from a pre-extracted job using full constraint-solve preflight.
bool dispatch_solve_island_job_guarded(RigidBodySoA& bodies,
                                       const IslandSolveJob& job,
                                       SolverWorkBuffers& workBuffers,
                                       const std::vector<DistanceConstraint>& distanceConstraints,
                                       f32 dt,
                                       f32 contactCompliance,
                                       const std::function<f32(const RigidBodySoA&, u32)>& invMassFn);

/// Guarded dispatch from a pre-extracted job with constraint-solve preflight outcome.
IslandDispatchResult dispatch_solve_island_job_guarded_result(
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
    const std::vector<IslandSolveJob>& jobs,
/// Summarize solveable vs all-sleeping/stale/empty islands for batch guards.
IslandSolveableStats compute_island_solveable_stats(
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints);

/// Graph-level solveable preflight; sets `skipped` when nothing can solve.
IslandSolveableGraphPreflight preflight_island_solveable_graph(

/// Early-out guard when no island passes solveable preflight.
bool should_skip_island_solveable_graph(const ContactIslandGraph& graph,

/// Collect island indices that pass dispatch, sleep, and constraint-solve preflights.
std::vector<u32> collect_solveable_island_indices(

/// Batch guarded dispatch over solveable islands; wakes mixed-sleep islands first.
IslandBatchDispatchResult dispatch_all_solveable_islands_result(
/// Guarded dispatch with wake preflight; wakes mixed-sleep islands before solve.
bool dispatch_solve_island_with_wake_guarded(RigidBodySoA& bodies,
                                             SolverWorkBuffers& workBuffers,
                                             const std::vector<DistanceConstraint>& distanceConstraints,
                                             f32 dt,
                                             f32 contactCompliance,
                                             const std::function<f32(const RigidBodySoA&, u32)>& invMassFn);

/// Guarded dispatch with wake preflight and explicit skip/solve outcome.
IslandDispatchResult dispatch_solve_island_with_wake_result(
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
/// Batch guarded dispatch over solveable islands; returns count actually solved.
u32 dispatch_all_solveable_islands_guarded(
/// Batch guarded dispatch over solveable islands only (refs + bodies + sleep guards).
IslandBatchDispatchResult dispatch_all_solveable_islands_result(

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
/// Guarded island solve; skips all-sleeping islands and islands with no movable bodies.

/// Guarded island solve with wake-then-solve for mixed-sleep islands.
bool solve_island_job_with_wake_guarded(RigidBodySoA& bodies,
/// Guarded island solve; skips stale refs, all-sleeping, and no-movable-body islands.
/// Guarded island solve; skips stale refs, all-sleeping islands, and invalid dt.
/// Guarded island solve using constraint-ref + body participation preflight (B4.4 deepen follow-up pass).

/// Guarded dispatch that skips all-sleeping islands before solve (B4.4 deepen follow-up pass).
bool dispatch_solve_island_with_sleep_guard(RigidBodySoA& bodies,

/// Batch guarded dispatch with sleep-skip counts (B4.4 deepen follow-up pass).
IslandBatchSleepDispatchResult dispatch_all_islands_with_sleep_guard_result(
/// Guarded island solve with extended refs/bodies/sleep preflight; valid paths delegate to `solve_island_job`.

/// Guarded per-island pipeline: wake mixed sleepers then solve; returns false when skipped.
IslandPipelineDispatchResult dispatch_island_pipeline_guarded(

/// Batch guarded pipeline dispatch over solveable islands; returns count actually solved.
u32 dispatch_all_islands_pipeline_guarded(

/// Batch guarded pipeline dispatch with explicit wake/solve/skip counts.
IslandBatchPipelineDispatchResult dispatch_all_islands_pipeline_result(

/// Collect island indices that are dispatchable and not all-sleeping (pipeline solve prep).
std::vector<u32> collect_solveable_island_indices(const ContactIslandGraph& graph,
                                                  const RigidBodySoA& bodies);

/// Count islands that pass pipeline solve preflight.
u32 count_solveable_islands(const ContactIslandGraph& graph, const RigidBodySoA& bodies);

/// True when at least one island can wake-and-solve this substep.
bool has_solveable_islands(const ContactIslandGraph& graph, const RigidBodySoA& bodies);

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

/// Classify build reject reason from build preflight diagnostics.
IslandGraphBuildRejectReason classifyIslandGraphBuildReject(const IslandBuildPreflight& preflight);

/// Build preflight with optional reject-reason output (B4.4 deepen pass).
bool tryPreflightIslandBuild(u32 bodyCount,
                             const std::vector<narrowphase::ContactManifold>& contacts,
                             const std::vector<DistanceConstraint>& distanceConstraints,
                             IslandGraphBuildRejectReason& reason);

/// Classify dispatch reject reason from dispatch preflight diagnostics.
IslandDispatchRejectReason classifyIslandDispatchReject(const IslandDispatchPreflight& preflight);

/// Classify per-job dispatch reject reason from job preflight diagnostics.
IslandSolveJobRejectReason classifyIslandSolveJobReject(const IslandSolveJobPreflight& preflight);

/// Classify constraint solve reject reason from solve preflight diagnostics.
IslandSolveRejectReason classifyIslandConstraintSolveReject(const IslandConstraintSolvePreflight& preflight);

/// Classify sleep reject reason from sleep preflight diagnostics.
IslandSleepRejectReason classifyIslandSleepReject(const IslandSleepPreflight& preflight);

/// Classify graph-level sleep reject reason from sleep graph preflight diagnostics.
IslandSleepGraphRejectReason classifyIslandSleepGraphReject(const IslandSleepGraphPreflight& preflight);

/// Classify wake reject reason from wake preflight diagnostics.
IslandWakeRejectReason classifyIslandWakeReject(const IslandWakePreflight& preflight);

/// Classify graph-level wake reject reason from wake graph preflight diagnostics.
IslandWakeGraphRejectReason classifyIslandWakeGraphReject(const IslandWakeGraphPreflight& preflight);

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

/// True when at least one in-range contact or distance constraint exists for build.
bool has_in_range_constraints(
    u32 bodyCount,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints);

/// Non-mutating build predicate — inverse of `should_skip_island_build`.
bool should_run_island_build(u32 bodyCount,
                             const std::vector<narrowphase::ContactManifold>& contacts,
                             const std::vector<DistanceConstraint>& distanceConstraints);

/// True when at least one in-range contact or distance constraint exists for build.
bool has_in_range_constraints(
    u32 bodyCount,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints);

/// True when at least one in-range contact or distance constraint exists for build.
bool has_in_range_constraints(
    u32 bodyCount,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints);

/// Non-mutating island build skip predicate — mirrors `preflight_island_build`.
bool can_skip_island_build(u32 bodyCount,
                           const std::vector<narrowphase::ContactManifold>& contacts,
                           const std::vector<DistanceConstraint>& distanceConstraints);

/// Non-mutating island build predicate — inverse of `can_skip_island_build`.
bool should_run_island_build(u32 bodyCount,
                             const std::vector<narrowphase::ContactManifold>& contacts,
                             const std::vector<DistanceConstraint>& distanceConstraints);

/// True when at least one in-range contact or distance constraint exists for build.
bool has_in_range_constraints(
    u32 bodyCount,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints);

/// Non-mutating build predicate — mirrors `preflight_island_build` (B4.4 deepen follow-up pass).
bool should_run_island_build(u32 bodyCount,
                             const std::vector<narrowphase::ContactManifold>& contacts,
                             const std::vector<DistanceConstraint>& distanceConstraints);

/// Non-mutating build predicate — mirrors `preflight_island_build` (B4.4 deepen follow-up pass).
bool should_run_island_build(u32 bodyCount,
                             const std::vector<narrowphase::ContactManifold>& contacts,
                             const std::vector<DistanceConstraint>& distanceConstraints);

/// Non-mutating build predicate — mirrors `preflight_island_build` (B4.4 deepen follow-up pass).
bool should_run_island_build(u32 bodyCount,
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

/// Build only when island build preflight passes; no-op otherwise (B4.5 deepen follow-up pass).
bool build_island_graph_with_preflight(ContactIslandGraph& graph,

/// Guarded island graph build with explicit build/skip outcome.
IslandBuildResult build_island_graph_result(ContactIslandGraph& graph,

/// Guarded island graph build with explicit skip/unsafe/degenerate outcome.

/// Guarded island graph build with explicit skip/unsafe/built outcome.




/// Guarded island graph build with explicit preflight output (B4.4 deepen follow-up pass).
                                       IslandBuildPreflight& outPreflight);

/// Preflight body participation by island index; out-of-range indices are marked skipped.
IslandSolveBodiesPreflight preflight_island_solve_bodies_by_index(const ContactIslandGraph& graph,
                                                                  u32 islandIndex,
                                                                  const RigidBodySoA& bodies);

/// Early-out guard when an island has no movable bodies to solve.
bool should_skip_island_solve_bodies(const ContactIslandGraph::Island& island,
/// Returns the first reject reason for one island constraint solve, or `None` when solve may proceed.
IslandConstraintSolveRejectReason island_constraint_solve_reject_reason(
    const ContactIslandGraph::Island& island,
    const RigidBodySoA& bodies,
    f32 dt);

/// Returns true when `island_constraint_solve_reject_reason` matches `expected` (B4.4 deepen follow-up pass).
bool island_constraint_solve_rejects_for_reason(
    f32 dt,
    IslandConstraintSolveRejectReason expected);

/// Combined constraint-ref + body participation preflight for one island solve pass.
IslandConstraintSolvePreflight preflight_island_constraint_solve(

/// Early-out guard when refs or body participation block island constraint solve.
bool should_skip_island_constraint_solve(const ContactIslandGraph::Island& island,

/// Constraint-solve preflight by island index; out-of-range indices are marked skipped.
IslandConstraintSolvePreflight preflight_island_constraint_solve_by_index(
    const ContactIslandGraph& graph,

/// Early-out guard for constraint solve by island index (empty, out-of-range, or blocked).
bool should_skip_island_constraint_solve_by_index(const ContactIslandGraph& graph,

/// Summarize solveable vs blocked islands for graph-level constraint-solve guards.
IslandConstraintSolveGraphStats compute_island_constraint_solve_stats(

/// Count islands that pass per-island constraint-solve preflight.
u32 count_constraint_solveable_islands(const ContactIslandGraph& graph,

/// True when at least one island can run constraint solve this substep.
bool has_constraint_solveable_islands(const ContactIslandGraph& graph,

/// Graph-level constraint-solve preflight; sets `skipped` when nothing can solve.
IslandConstraintSolveGraphPreflight preflight_island_constraint_solve_graph(

/// Early-out guard when every constrained island is blocked from constraint solve.
bool should_skip_island_constraint_solve_graph(

/// Collect island indices that pass per-island constraint-solve preflight.
std::vector<u32> collect_constraint_solveable_island_indices(

/// Combined wake + sleep + constraint-solve preflight for one island solve pass.
IslandSolvePipelinePreflight preflight_island_solve_pipeline(

/// Combined wake + sleep + constraint-solve preflight by island index.
IslandSolvePipelinePreflight preflight_island_solve_pipeline_by_index(

/// Early-out guard when wake/sleep/refs/bodies block island solve pipeline.
bool should_skip_island_solve_pipeline(const ContactIslandGraph::Island& island,

/// Guarded island solve using full constraint-solve preflight; `solve_island_job` unchanged.
bool solve_island_job_guarded(RigidBodySoA& bodies,
                              SolverWorkBuffers& workBuffers,

/// Wake sleepers then guarded solve when pipeline preflight allows work.
bool solve_island_job_with_wake_guarded(RigidBodySoA& bodies,

/// Guarded dispatch from a pre-extracted job using constraint-solve preflight.
bool dispatch_solve_island_job_guarded(RigidBodySoA& bodies,
                                       const IslandSolveJob& job,

/// Wake sleepers then guarded dispatch for one island index.
bool dispatch_solve_island_with_wake_guarded(RigidBodySoA& bodies,

/// Batch wake + guarded dispatch over constraint-solveable islands.
u32 dispatch_all_islands_with_wake_guarded(

/// Batch wake + guarded dispatch with explicit skip/solve counts.
IslandBatchDispatchResult dispatch_all_islands_with_wake_result(

/// Combined constraint-solve preflight by island index; out-of-range indices are marked skipped.
/// Combined constraint-ref + body participation preflight by island index.
/// Combined constraint solve preflight by island index; out-of-range indices are marked skipped.

bool should_skip_island_constraint_solve_index(const ContactIslandGraph& graph,

/// Guarded island constraint solve; returns false when preflight blocks solve.

/// Guarded island constraint solve with explicit skip/solve outcome.
IslandConstraintSolveResult solve_island_job_result(RigidBodySoA& bodies,

/// Combined solve pipeline preflight for one island (dispatch + refs + bodies + sleep).
IslandSolvePipelinePreflight preflight_island_solve_pipeline(const ContactIslandGraph& graph,

/// Guarded dispatch combining wake + constraint-solve preflights before solve.
bool dispatch_solve_island_with_solve_guards(RigidBodySoA& bodies,

/// Guarded dispatch with explicit skip/solve outcome and solve-pipeline preflights.
IslandDispatchResult dispatch_solve_island_with_solve_guards_result(

/// Preflight constraint solve by island index; out-of-range indices are marked skipped.

/// Preflight wake + sleep + constraint solve for one island pipeline pass.

/// Preflight solve pipeline by island index; out-of-range indices are marked skipped.

/// Early-out guard when sleep state or constraint refs block island solve pipeline.

/// Guarded island solve pipeline: wake mixed sleepers then solve when preflight allows.
bool solve_island_job_pipeline_guarded(RigidBodySoA& bodies,

/// Guarded island solve pipeline by island index.
bool solve_island_job_pipeline_by_index_guarded(RigidBodySoA& bodies,

/// Collect island indices that pass constraint-solve preflight.

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

u32 count_solveable_islands(const ContactIslandGraph& graph,

bool has_solveable_islands(const ContactIslandGraph& graph,


std::vector<u32> collect_solveable_island_indices(


/// Early-out guard for constraint solve by island index (empty or out-of-range).


/// Guarded constraint solve by island index; returns false for out-of-range or blocked preflight.
bool dispatch_solve_island_constraint_guarded(RigidBodySoA& bodies,

/// Resolve constraints only when deepen preflight passes; returns false when skipped (B4.5 deepen follow-up pass).
bool solve_island_job_with_preflight(RigidBodySoA& bodies,

/// Guarded dispatch with deepen preflight; skips stale refs, immovable, and all-sleeping islands (B4.5 deepen follow-up pass).
bool dispatch_solve_island_with_preflight(

/// Non-mutating constraint-solve predicate — inverse of `should_skip_island_constraint_solve`.
bool should_run_island_constraint_solve(const ContactIslandGraph::Island& island,

/// Preflight one island job including body participation and constraint coverage.
IslandConstraintSolveJobPreflight preflight_solve_island_job_with_bodies(

/// Early-out guard when job, refs, or body participation block island solve.
bool should_skip_solve_island_job_with_bodies(const IslandSolveJob& job,

/// Guarded island solve using full constraint + body preflight; skips all-sleeping islands.

/// Guarded dispatch with body-participation preflight and explicit skip/solve outcome.
IslandDispatchResult dispatch_solve_island_with_bodies_result(

/// Collect island indices that pass constraint + body solve preflight.

/// Batch guarded dispatch skipping all-sleeping and non-movable islands.
IslandBatchBodiesDispatchResult dispatch_all_islands_with_bodies_result(


/// Summarize solveable vs all-sleeping vs stale-ref islands for batch guards.

/// Graph-level constraint solve preflight; sets `skipped` when nothing can solve.

/// Early-out guard when every constrained island fails constraint-solve preflight.

/// Collect island indices that pass constraint-solve preflight (parallel dispatch prep).

/// Guarded island solve with constraint-ref and body participation preflight.

/// Guarded dispatch with constraint-solve preflight before solve.





/// Early-out guard when every constrained island is blocked from solving.

/// Collect island indices that pass constraint-ref + body participation preflight.



/// Combined constraint, body, and sleep preflight for one island solve pass.
IslandSolvePassPreflight preflight_island_solve_pass(

/// Combined solve-pass preflight by island index; out-of-range indices are marked skipped.
IslandSolvePassPreflight preflight_island_solve_pass_by_index(

/// Early-out guard when constraint refs, body participation, or sleep block island solve.
bool should_skip_island_solve_pass(const ContactIslandGraph::Island& island,

/// Guarded island solve; returns false when solve-pass preflight blocks work.

/// Collect island indices that are dispatchable and not all-sleeping.
std::vector<u32> collect_solveable_island_indices(const ContactIslandGraph& graph,

/// Guarded wake with explicit skip/activate outcome.
IslandWakeResult wake_island_sleepers_result(RigidBodySoA& bodies,
                                             u32 islandIndex);

/// Wake sleeping neighbors then guarded-solve one island; returns false when solve is skipped.

/// Wake-then-solve dispatch with explicit wake/solve/skip outcome.
IslandWakeAndSolveResult dispatch_solve_island_with_wake_result(

/// Batch wake wakeable islands then guarded-dispatch solveable islands.

/// Batch wake-then-dispatch with explicit wake and dispatch counts.
IslandWakeAndDispatchResult dispatch_all_islands_with_wake_result(



/// Guarded island solve; returns false when constraint/sleep preflight blocks solve.

/// Preflight wake-then-solve for one island; sets `skipped` for empty islands.
IslandWakeThenSolvePreflight preflight_wake_then_solve_island(

/// Preflight wake-then-solve by island index; out-of-range indices are marked skipped.
IslandWakeThenSolvePreflight preflight_wake_then_solve_island_by_index(

/// Early-out guard when wake-then-solve preflight blocks dispatch.
bool should_skip_wake_then_solve_island(const ContactIslandGraph::Island& island,

/// Guarded wake-then-solve: wakes mixed-island sleepers then solves when preflight allows.

/// Collect island indices that pass constraint-solve and nonsleeping preflight.

/// Batch guarded dispatch over solveable nonsleeping islands.
IslandNonsleepingDispatchResult dispatch_all_nonsleeping_islands_result(

/// Batch guarded dispatch over solveable nonsleeping islands; returns count solved.
u32 dispatch_all_nonsleeping_islands(RigidBodySoA& bodies,

/// Early-out guard for constraint solve by island index (empty, out-of-range, or blocked bodies).
















/// Preflight constraint-solve + sleep state for one island dispatch pass.
IslandSolveDispatchPreflight preflight_island_solve_dispatch(

/// Preflight constraint-solve + sleep state by island index; out-of-range indices are marked skipped.
IslandSolveDispatchPreflight preflight_island_solve_dispatch_by_index(

/// Early-out guard when constraint refs, body participation, or sleep block island dispatch.
bool should_skip_island_solve_dispatch(const ContactIslandGraph::Island& island,

/// Summarize solveable vs all-sleeping/blocked islands for batch guards.
IslandSolveableStats compute_island_solveable_stats(

/// Count islands that pass per-island solve-dispatch preflight.

/// True when at least one island can dispatch constraint solve this substep.

/// Collect island indices that pass per-island solve-dispatch preflight.

/// Preflight graph dispatch including timestep validity and sleep batching.
IslandSleepSolveDispatchPreflight preflight_island_sleep_dispatch(const ContactIslandGraph& graph,

/// Early-out guard combining graph dispatch preflight, timestep validity, and sleep batching.
bool should_skip_island_sleep_dispatch(const ContactIslandGraph& graph,


/// Guarded dispatch with constraint-solve + sleep preflight; optional wake before solve.
bool dispatch_solve_island_guarded(RigidBodySoA& bodies,
                                   const std::function<f32(const RigidBodySoA&, u32)>& invMassFn,
                                   bool wakeSleepers = false);

/// Guarded dispatch with explicit skip/solve outcome and optional wake before solve.
IslandDispatchResult dispatch_solve_island_guarded_result(

/// Batch guarded dispatch over solveable islands; skips all-sleeping and blocked islands.
IslandBatchDispatchResult dispatch_solveable_islands_result(

/// Batch guarded dispatch over solveable islands; returns count actually solved.
u32 dispatch_solveable_islands(RigidBodySoA& bodies,

/// Combined solve dispatch preflight for one island (refs, bodies, sleep, wake).

/// Combined solve dispatch preflight by island index; out-of-range indices are marked skipped.

/// Early-out guard when constraint, body, or sleep preflights block island solve dispatch.

/// Guarded island solve using full constraint/body/sleep preflight (opt-in over `solve_island_job`).

/// Guarded dispatch: wake mixed-island sleepers, then solve with full preflight.

/// Guarded dispatch with explicit skip/solve/wake outcome.
IslandGuardedDispatchResult dispatch_solve_island_guarded_result(

/// Collect island indices that pass combined solve dispatch preflight.

/// Batch guarded dispatch: wake wakeable islands, skip all-sleeping, solve the rest.
u32 dispatch_all_islands_guarded(RigidBodySoA& bodies,

/// Batch guarded dispatch with explicit skip/solve/wake counts.
IslandBatchGuardedDispatchResult dispatch_all_islands_guarded_result(



/// Combined sleep + wake + constraint-solve preflight for one island pipeline pass.

/// Combined pipeline preflight by island index; out-of-range indices are marked skipped.

/// Early-out guard when pipeline preflight blocks island solve (all-sleeping or no movable bodies).

/// Early-out guard for pipeline preflight by island index.
bool should_skip_island_solve_pipeline_by_index(const ContactIslandGraph& graph,

/// Collect island indices that pass constraint-solve preflight (parallel solve prep stub).

/// Collect island indices that pass pipeline preflight (nonsleeping + constraint-solveable).
std::vector<u32> collect_pipeline_solveable_island_indices(

/// Guarded island solve using constraint-ref + body participation preflight.

/// Guarded island solve with explicit skip/solve outcome.
IslandConstraintSolveResult solve_island_job_guarded_result(

/// Wake sleeping neighbors then guarded-solve when pipeline preflight allows.

/// Guarded dispatch: wake sleepers when needed, then constraint-solve guarded pass.

/// Guarded wake+dispatch with explicit skip/solve outcome.
IslandDispatchResult dispatch_solve_island_with_wake_result(

/// Batch guarded wake + dispatch over pipeline-solveable islands.

/// Batch guarded wake + dispatch with explicit woke/solved/skipped counts.
IslandBatchWakeDispatchResult dispatch_all_islands_with_wake_result(




/// Graph-level constraint-solve preflight; sets `skipped` when no island can solve.



/// Guarded island solve; returns false when constraint-solve preflight blocks work.

/// Guarded dispatch with constraint-solve preflight; skips blocked islands.

/// Combined dispatch + wake preflight for one island index.
IslandDispatchWithWakePreflight preflight_dispatch_with_wake(const ContactIslandGraph& graph,

/// Guarded dispatch that wakes mixed-sleep islands before solve when needed.
bool dispatch_solve_island_wake_guarded(RigidBodySoA& bodies,

/// Batch guarded wake with explicit skip/woke counts.
IslandWakeBatchResult wake_all_island_sleepers_result(RigidBodySoA& bodies,
                                                      const ContactIslandGraph& graph);


/// Solve-pass preflight by island index; out-of-range indices are marked skipped.

/// Early-out guard when constraint, body, or sleep state blocks island solve.

/// Graph-level nonsleeping dispatch preflight including timestep validity.
IslandNonsleepingDispatchPreflight preflight_nonsleeping_island_dispatch(const ContactIslandGraph& graph,

/// Early-out guard when no nonsleeping islands can be dispatched.
bool should_skip_nonsleeping_island_dispatch(const ContactIslandGraph& graph,

/// Guarded island solve using constraint + sleep preflights; valid paths delegate to `solve_island_job`.

/// Guarded island solve that wakes sleepers first when mixed sleep state requires it.

/// Guarded dispatch with constraint + sleep preflights; valid paths delegate to `dispatch_solve_island`.

/// Guarded dispatch that wakes sleepers before solve when required.

/// Batch guarded dispatch over nonsleeping islands; returns count of islands actually solved.
u32 dispatch_nonsleeping_islands_guarded(RigidBodySoA& bodies,

/// Batch guarded nonsleeping dispatch with explicit skip/solve counts.
IslandBatchDispatchResult dispatch_nonsleeping_islands_result(








/// Guarded island constraint solve with explicit preflight output (B4.4 deepen follow-up pass).
                                     IslandConstraintSolvePreflight& outPreflight);

/// Guarded in-range-only island graph build; skips out-of-range constraints without aborting.
bool build_island_graph_in_range_guarded(ContactIslandGraph& graph,
                                         ContactIslandGraphBuildStats* outStats = nullptr);


/// Summarize solveable vs blocked islands for constraint-solve batch guards.


bool should_skip_island_constraint_solve_graph(const ContactIslandGraph& graph,

/// Preflight constraint-solve by island index; out-of-range indices are marked skipped.

/// Early-out guard for constraint-solve by island index (empty, out-of-range, or blocked).


/// Guarded island solve; returns false when preflight blocks constraint solve.


IslandBuildOutcome build_island_graph_result(ContactIslandGraph& graph,
                                IslandGraphBuildRejectReason* reason = nullptr);

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

/// Classify build preflight into a reject reason (B4.4 deepen follow-up).
IslandGraphBuildRejectReason classify_island_build_reject(const IslandBuildPreflight& preflight);

/// Classify graph dispatch preflight into a reject reason (B4.4 deepen follow-up).
IslandDispatchRejectReason classify_island_dispatch_reject(const IslandDispatchPreflight& preflight);

/// Classify per-job solve preflight into a reject reason (B4.4 deepen follow-up).
IslandSolveRejectReason classify_island_solve_job_reject(const IslandSolveJobPreflight& preflight);

/// Classify combined constraint solve preflight into a reject reason (B4.4 deepen follow-up).
IslandSolveRejectReason classify_island_constraint_solve_reject(const IslandConstraintSolvePreflight& preflight);

/// Classify sleep preflight into a reject/skip reason (B4.4 deepen follow-up).
IslandSleepRejectReason classify_island_sleep_reject(const IslandSleepPreflight& preflight);

/// Classify graph sleep preflight into a reject reason (B4.4 deepen follow-up).
IslandSleepRejectReason classify_island_sleep_graph_reject(const IslandSleepGraphPreflight& preflight);

/// Classify wake preflight into a reject reason (B4.4 deepen follow-up).
IslandWakeRejectReason classify_island_wake_reject(const IslandWakePreflight& preflight);

/// Classify graph wake preflight into a reject reason (B4.4 deepen follow-up).
IslandWakeRejectReason classify_island_wake_graph_reject(const IslandWakeGraphPreflight& preflight);

/// Build preflight with optional reject-reason output (B4.4 deepen follow-up).
bool preflight_island_build_ready(
    u32 bodyCount,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints,
    IslandGraphBuildRejectReason* reason = nullptr);

bool try_preflight_island_build(
    u32 bodyCount,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints,
    IslandGraphBuildRejectReason& reason);

/// Dispatch preflight with optional reject-reason output (B4.4 deepen follow-up).
bool preflight_island_dispatch_ready(const ContactIslandGraph& graph,
                                     f32 dt,
                                     IslandDispatchRejectReason* reason = nullptr);

bool try_preflight_island_dispatch(const ContactIslandGraph& graph,
                                   f32 dt,
                                   IslandDispatchRejectReason& reason);

/// Per-job solve preflight with optional reject-reason output (B4.4 deepen follow-up).
bool preflight_solve_island_job_ready(const IslandSolveJob& job,
                                      f32 dt,
                                      IslandSolveRejectReason* reason = nullptr);

bool try_preflight_solve_island_job(const IslandSolveJob& job,
                                    f32 dt,
                                    IslandSolveRejectReason& reason);

/// Sleep preflight with optional reject-reason output (B4.4 deepen follow-up).
bool preflight_island_sleep_ready(const ContactIslandGraph::Island& island,
                                  const RigidBodySoA& bodies,
                                  IslandSleepRejectReason* reason = nullptr);

bool try_preflight_island_sleep(const ContactIslandGraph::Island& island,
                                const RigidBodySoA& bodies,
                                IslandSleepRejectReason& reason);

/// Wake preflight with optional reject-reason output (B4.4 deepen follow-up).
bool preflight_island_wake_ready(const ContactIslandGraph::Island& island,
                                 const RigidBodySoA& bodies,
                                 IslandWakeRejectReason* reason = nullptr);

bool try_preflight_island_wake(const ContactIslandGraph::Island& island,
                               const RigidBodySoA& bodies,
                               IslandWakeRejectReason& reason);

/// Preflight body participation for one island solve pass.
IslandSolveBodiesPreflight preflight_island_solve_bodies(const ContactIslandGraph::Island& island,
                                                         const RigidBodySoA& bodies);

/// Early-out guard when an island has no movable bodies to solve.
bool should_skip_island_solve_bodies(const ContactIslandGraph::Island& island,
                                     const RigidBodySoA& bodies);

/// Returns the first reject reason for one island sleep preflight, or `None` when not all-sleeping.
IslandSleepRejectReason island_sleep_reject_reason(const ContactIslandGraph::Island& island,
                                                   const RigidBodySoA& bodies);

/// Returns true when `island_sleep_reject_reason` matches `expected` (B4.4 deepen follow-up pass).
bool island_sleep_rejects_for_reason(const ContactIslandGraph::Island& island,
                                     const RigidBodySoA& bodies,
                                     IslandSleepRejectReason expected);

/// Returns the first reject reason for one island wake preflight, or `None` when wake may proceed.
IslandWakeRejectReason island_wake_reject_reason(const ContactIslandGraph::Island& island,
                                                 const RigidBodySoA& bodies);

/// Returns true when `island_wake_reject_reason` matches `expected` (B4.4 deepen follow-up pass).
bool island_wake_rejects_for_reason(const ContactIslandGraph::Island& island,
                                    const RigidBodySoA& bodies,
                                    IslandWakeRejectReason expected);

/// Preflight constraint-solve by island index; out-of-range indices are marked skipped.
IslandConstraintSolvePreflight preflight_island_constraint_solve_by_index(
    const ContactIslandGraph& graph,
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

/// Graph-level constraint-solve preflight; sets `skipped` when no island can solve.
IslandConstraintSolveGraphPreflight preflight_island_constraint_solve_graph(
    const ContactIslandGraph& graph,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints);

/// Early-out guard when every constrained island is blocked from solving.
bool should_skip_island_constraint_solve_graph(
    const ContactIslandGraph& graph,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints);

/// Count islands that pass per-island constraint-solve preflight.
u32 count_solveable_islands(const ContactIslandGraph& graph,
                            const RigidBodySoA& bodies,
                            const std::vector<narrowphase::ContactManifold>& contacts,
                            const std::vector<DistanceConstraint>& distanceConstraints);

/// True when at least one island can run constraint solve this substep.
bool has_solveable_islands(const ContactIslandGraph& graph,
                           const RigidBodySoA& bodies,
                           const std::vector<narrowphase::ContactManifold>& contacts,
                           const std::vector<DistanceConstraint>& distanceConstraints);

/// Collect island indices that pass per-island constraint-solve preflight.
std::vector<u32> collect_solveable_island_indices(
    const ContactIslandGraph& graph,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints);

/// Combined wake + constraint-solve preflight for one island.
IslandWakeAndSolvePreflight preflight_island_wake_and_solve(
    const ContactIslandGraph::Island& island,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints);

/// Combined wake + constraint-solve preflight by island index.
IslandWakeAndSolvePreflight preflight_island_wake_and_solve_by_index(
    const ContactIslandGraph& graph,
    u32 islandIndex,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints);

/// Guarded island solve; skips stale refs, all-sleeping, and no-movable islands.
bool solve_island_job_guarded(RigidBodySoA& bodies,
                              const ContactIslandGraph::Island& island,
                              SolverWorkBuffers& workBuffers,
                              const std::vector<DistanceConstraint>& distanceConstraints,
                              f32 dt,
                              f32 contactCompliance,
                              const std::function<f32(const RigidBodySoA&, u32)>& invMassFn);

/// Guarded dispatch with constraint-solve preflight; skips blocked islands.
bool dispatch_solve_island_guarded(RigidBodySoA& bodies,

/// Guarded dispatch from a pre-extracted job with constraint-solve preflight.
bool dispatch_solve_island_job_guarded(RigidBodySoA& bodies,
                                     const IslandSolveJob& job,

/// Guarded dispatch with explicit skip/solve outcome and constraint-solve preflight.
IslandDispatchResult dispatch_solve_island_guarded_result(
    RigidBodySoA& bodies,

/// Wake sleepers then guarded-dispatch one island; returns false when dispatch is skipped.
bool dispatch_solve_island_with_wake_guarded(RigidBodySoA& bodies,

/// Wake sleepers then guarded-dispatch from a pre-extracted job.
bool dispatch_solve_island_job_with_wake_guarded(RigidBodySoA& bodies,

/// Batch guarded dispatch over solveable islands; returns count actually solved.
u32 dispatch_all_solveable_islands_guarded(

/// Batch guarded dispatch with wake-then-solve over solveable islands.
u32 dispatch_all_solveable_islands_with_wake_guarded(

/// Batch guarded dispatch with explicit skip/solve counts over solveable islands.
IslandBatchDispatchResult dispatch_all_solveable_islands_result(

/// Batch wake-then-solve dispatch with explicit skip/solve counts.
IslandBatchDispatchResult dispatch_all_solveable_islands_with_wake_result(

/// Combined constraint-solve preflight by island index; out-of-range indices are marked skipped.
/// Combined constraint-solve preflight by island index; out-of-range indices are marked skipped (B4.4 deepen follow-up pass).
IslandConstraintSolvePreflight preflight_island_constraint_solve_by_index(
    const ContactIslandGraph& graph,
    u32 islandIndex,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints);

/// Early-out guard for constraint-solve by island index (empty, out-of-range, or blocked bodies/refs).
bool should_skip_island_constraint_solve_by_index(const ContactIslandGraph& graph,
                                                  u32 islandIndex,
                                                  const RigidBodySoA& bodies,
                                                  const std::vector<narrowphase::ContactManifold>& contacts,
                                                  const std::vector<DistanceConstraint>& distanceConstraints);

/// Guarded island constraint solve; returns false when refs or body participation block solve.
bool solve_island_job_guarded(RigidBodySoA& bodies,
                              const ContactIslandGraph::Island& island,
                              SolverWorkBuffers& workBuffers,
                              const std::vector<DistanceConstraint>& distanceConstraints,
                              f32 dt,
                              f32 contactCompliance,
                              const std::function<f32(const RigidBodySoA&, u32)>& invMassFn);

/// Guarded dispatch with full constraint-solve preflight (refs + movable bodies).
bool dispatch_solve_island_constraint_guarded(RigidBodySoA& bodies,
                                              const ContactIslandGraph& graph,

/// Guarded dispatch with explicit skip/solve outcome and constraint-solve preflight.
IslandDispatchResult dispatch_solve_island_constraint_result(
    RigidBodySoA& bodies,

/// Combined wake + constraint-solve preflight for one island.
IslandWakeThenSolvePreflight preflight_wake_then_solve_island(const ContactIslandGraph::Island& island,

/// Combined wake + constraint-solve preflight by island index; out-of-range indices are marked skipped.
IslandWakeThenSolvePreflight preflight_wake_then_solve_island_by_index(

/// Guarded wake-then-solve for one island; wakes sleepers when needed, then solves if allowed.
bool dispatch_solve_island_after_wake_guarded(RigidBodySoA& bodies,

/// Guarded wake-then-solve with explicit skip/solve outcome.
IslandDispatchResult dispatch_solve_island_after_wake_result(

/// Summarize solveable vs all-sleeping/stale islands for batch guards.
IslandSolveableStats compute_island_solveable_stats(const ContactIslandGraph& graph,

/// Graph-level solveable-island preflight; sets `skipped` when nothing can be solved.
IslandSolveableGraphPreflight preflight_island_solveable_graph(

/// Early-out guard when every constrained island is all-sleeping or has stale refs.
bool should_skip_island_solveable_graph(const ContactIslandGraph& graph,

/// Collect island indices that pass full constraint-solve preflight.
std::vector<u32> collect_solveable_island_indices(const ContactIslandGraph& graph,

/// Batch guarded dispatch over solveable islands; returns count of islands actually solved.
u32 dispatch_all_solveable_islands_guarded(

/// Batch guarded dispatch over solveable islands with explicit skip/solve counts.
IslandBatchDispatchResult dispatch_all_solveable_islands_result(

/// Summarize solveable vs blocked islands for graph-level constraint-solve guards.
IslandConstraintSolveGraphStats compute_island_constraint_solve_stats(

/// Graph-level constraint-solve preflight; sets `skipped` when no island can solve.
IslandConstraintSolveGraphPreflight preflight_island_constraint_solve_graph(

/// Early-out guard when every constrained island is blocked from solving.
bool should_skip_island_constraint_solve_graph(

/// Collect island indices that pass constraint-ref + body participation preflight.
std::vector<u32> collect_constraint_solveable_island_indices(

/// Guarded island solve with constraint-ref + body participation preflight.

/// Guarded dispatch with constraint-ref + body participation preflight.

/// Batch guarded dispatch over constraint-solveable islands; returns count actually solved.
u32 dispatch_all_islands_constraint_guarded(



bool should_skip_island_constraint_solve_graph(const ContactIslandGraph& graph,

std::vector<u32> collect_solveable_island_indices(

/// Preflight constraint solve by island index; out-of-range indices are marked skipped.
IslandConstraintSolvePreflight preflight_island_constraint_solve_by_index(

/// Preflight body participation by island index; out-of-range indices are marked skipped.
IslandSolveBodiesPreflight preflight_island_solve_bodies_by_index(const ContactIslandGraph& graph,
                                                                  const RigidBodySoA& bodies);

/// Early-out guard for constraint solve by island index.
bool should_skip_island_constraint_solve_index(const ContactIslandGraph& graph,

/// Preflight constraint solve dispatch including timestep validity.
IslandConstraintSolveDispatchPreflight preflight_island_constraint_solve_dispatch(
    f32 dt);

/// Preflight constraint solve dispatch by island index.
IslandConstraintSolveDispatchPreflight preflight_island_constraint_solve_dispatch_by_index(

/// Early-out guard when refs, bodies, or dt block island constraint solve.
bool should_skip_island_constraint_solve_dispatch(

/// Guarded island constraint solve; returns false when preflight blocks solve.

/// Guarded island constraint solve with explicit skip/solve outcome.
IslandConstraintSolveResult solve_island_job_guarded_result(

/// Guarded island constraint solve by index with explicit skip/solve outcome.
IslandConstraintSolveResult solve_island_job_by_index_guarded_result(

/// Preflight pre-solve pipeline for one island (wake + sleep + constraint readiness).
IslandPreSolvePreflight preflight_island_pre_solve(

/// Preflight pre-solve pipeline by island index.
IslandPreSolvePreflight preflight_island_pre_solve_by_index(

/// Early-out guard when pre-solve pipeline cannot proceed.
bool should_skip_island_pre_solve(const ContactIslandGraph::Island& island,

/// Summarize pre-solveable islands for graph-level batch guards.
IslandPreSolveGraphStats compute_island_pre_solve_stats(

/// Graph-level pre-solve pipeline preflight.
IslandPreSolveGraphPreflight preflight_island_pre_solve_graph(

/// Collect island indices that pass pre-solve pipeline preflight.
std::vector<u32> collect_pre_solveable_island_indices(

/// Collect island indices that are dispatchable and pre-solveable.

/// Guarded pre-solve: wake sleepers then constraint-solve one island.
IslandConstraintSolveResult pre_solve_island_guarded_result(
/// Early-out guard when refs or body participation block island constraint solve.
bool should_skip_island_constraint_solve(const ContactIslandGraph::Island& island,

/// Preflight constraint solve by island index; out-of-range indices are marked skipped.
IslandConstraintSolvePreflight preflight_island_constraint_solve_by_index(
    const ContactIslandGraph& graph,
    u32 islandIndex,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints);

/// Preflight body participation by island index; out-of-range indices are marked skipped.
IslandSolveBodiesPreflight preflight_island_solve_bodies_by_index(const ContactIslandGraph& graph,
                                                                  u32 islandIndex,
                                                                  const RigidBodySoA& bodies);

/// Combined constraint, body, and sleep preflight for one island solve pass.
IslandFullSolvePreflight preflight_island_full_solve(const ContactIslandGraph::Island& island,
                                                     const RigidBodySoA& bodies,
                                                     const std::vector<narrowphase::ContactManifold>& contacts,
                                                     const std::vector<DistanceConstraint>& distanceConstraints,
                                                     f32 dt);

/// Combined constraint, body, and sleep preflight by island index.
IslandFullSolvePreflight preflight_island_full_solve_by_index(const ContactIslandGraph& graph,
                                                              u32 islandIndex,
                                                              const RigidBodySoA& bodies,
                                                              const std::vector<narrowphase::ContactManifold>& contacts,
                                                              const std::vector<DistanceConstraint>& distanceConstraints,
                                                              f32 dt);

/// Early-out guard when refs, body participation, sleep, or dt block island solve.
bool should_skip_island_full_solve(const ContactIslandGraph::Island& island,
                                   const RigidBodySoA& bodies,
                                   const std::vector<narrowphase::ContactManifold>& contacts,
                                   const std::vector<DistanceConstraint>& distanceConstraints,
                                   f32 dt);

/// Collect island indices that pass full solve preflight.
std::vector<u32> collect_solveable_island_indices(const ContactIslandGraph& graph,
                                                  const RigidBodySoA& bodies,
                                                  const std::vector<narrowphase::ContactManifold>& contacts,
                                                  const std::vector<DistanceConstraint>& distanceConstraints,
                                                  f32 dt);

/// Guarded island solve with constraint-ref, body, and sleep preflight.
bool solve_island_job_guarded(RigidBodySoA& bodies,
                              const ContactIslandGraph::Island& island,
                              SolverWorkBuffers& workBuffers,
                              const std::vector<DistanceConstraint>& distanceConstraints,
                              f32 dt,
                              f32 contactCompliance,
                              const std::function<f32(const RigidBodySoA&, u32)>& invMassFn);

/// Guarded dispatch with full solve preflight; wakes sleepers when required.
bool dispatch_solve_island_full_guarded(RigidBodySoA& bodies,
                                        const ContactIslandGraph& graph,
                                        u32 islandIndex,
                                        SolverWorkBuffers& workBuffers,
                                        const std::vector<DistanceConstraint>& distanceConstraints,
                                        f32 dt,
                                        f32 contactCompliance,
                                        const std::function<f32(const RigidBodySoA&, u32)>& invMassFn);

/// Guarded dispatch with explicit skip/solve outcome and full preflight.
IslandDispatchResult dispatch_solve_island_full_result(RigidBodySoA& bodies,
                                                       const ContactIslandGraph& graph,
                                                       u32 islandIndex,
                                                       SolverWorkBuffers& workBuffers,
                                                       const std::vector<DistanceConstraint>& distanceConstraints,
                                                       f32 dt,
                                                       f32 contactCompliance,
                                                       const std::function<f32(const RigidBodySoA&, u32)>& invMassFn);

/// Batch guarded full dispatch over solveable islands.
u32 dispatch_all_islands_full_guarded(RigidBodySoA& bodies,
                                      const ContactIslandGraph& graph,
                                      SolverWorkBuffers& workBuffers,
                                      const std::vector<DistanceConstraint>& distanceConstraints,
                                      f32 dt,
                                      f32 contactCompliance,
                                      const std::function<f32(const RigidBodySoA&, u32)>& invMassFn);

/// Batch guarded full dispatch with explicit skip/solve counts.
IslandBatchConstraintSolveResult dispatch_all_islands_full_result(
    RigidBodySoA& bodies,
    const ContactIslandGraph& graph,
    SolverWorkBuffers& workBuffers,
    const std::vector<DistanceConstraint>& distanceConstraints,
    f32 dt,
    f32 contactCompliance,
    const std::function<f32(const RigidBodySoA&, u32)>& invMassFn);

/// Wake sleepers then solve one island when full preflight passes.
bool solve_island_with_wake_guarded(RigidBodySoA& bodies,
                                    const ContactIslandGraph::Island& island,
                                    SolverWorkBuffers& workBuffers,
                                    const std::vector<DistanceConstraint>& distanceConstraints,
                                    f32 dt,
                                    f32 contactCompliance,
                                    const std::function<f32(const RigidBodySoA&, u32)>& invMassFn);

/// Non-mutating constraint-solve skip predicate — mirrors `preflight_island_constraint_solve`.
bool can_skip_island_constraint_solve(const ContactIslandGraph::Island& island,
                                      const RigidBodySoA& bodies,
                                      const std::vector<narrowphase::ContactManifold>& contacts,
                                      const std::vector<DistanceConstraint>& distanceConstraints);

/// Non-mutating constraint-solve predicate — inverse of `can_skip_island_constraint_solve`.
bool should_run_island_constraint_solve(const ContactIslandGraph::Island& island,
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

/// Combined constraint-ref + body participation preflight by island index.
IslandConstraintSolvePreflight preflight_island_constraint_solve_by_index(
    const ContactIslandGraph& graph,
    u32 islandIndex,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints);

/// Per-island solve pipeline preflight (job + constraint + sleep guards).
IslandSolvePipelinePreflight preflight_island_solve_pipeline(
    const IslandSolveJob& job,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints,
    f32 dt);

/// Per-island solve pipeline preflight by island index.
IslandSolvePipelinePreflight preflight_island_solve_pipeline_by_index(
    const ContactIslandGraph& graph,
    u32 islandIndex,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints,
    f32 dt);

/// Early-out guard when job, constraint, or sleep preflights block island solve.
bool should_skip_island_solve_pipeline(const IslandSolveJob& job,
                                       const RigidBodySoA& bodies,
                                       const std::vector<narrowphase::ContactManifold>& contacts,
                                       const std::vector<DistanceConstraint>& distanceConstraints,
                                       f32 dt);

/// Summarize solveable vs sleep/constraint-skipped islands for pipeline guards.
IslandPipelineDispatchStats compute_island_pipeline_dispatch_stats(
    const ContactIslandGraph& graph,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints,
    f32 dt);

/// Graph-level pipeline dispatch preflight; sets `skipped` when nothing can run.
IslandPipelineDispatchPreflight preflight_island_pipeline_dispatch(
    const ContactIslandGraph& graph,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints,
    f32 dt);

/// Early-out guard for the wake-then-solve pipeline when nothing is solveable.
bool should_skip_island_pipeline_dispatch(const ContactIslandGraph& graph,
                                          const RigidBodySoA& bodies,
                                          const std::vector<narrowphase::ContactManifold>& contacts,
                                          const std::vector<DistanceConstraint>& distanceConstraints,
                                          f32 dt);

/// Collect island indices that pass full solve pipeline preflight.
std::vector<u32> collect_solveable_island_indices(const ContactIslandGraph& graph,
                                                    const RigidBodySoA& bodies,
                                                    const std::vector<narrowphase::ContactManifold>& contacts,
                                                    const std::vector<DistanceConstraint>& distanceConstraints,
                                                    f32 dt);

/// Guarded wake with explicit skip/activate outcome.
IslandWakeResult wake_island_sleepers_result(RigidBodySoA& bodies,
                                               const ContactIslandGraph& graph,
                                               u32 islandIndex);

/// Guarded pipeline dispatch: wake sleepers then solve one island; returns false when skipped.
bool dispatch_solve_island_pipeline_guarded(RigidBodySoA& bodies,
                                            const ContactIslandGraph& graph,
                                            u32 islandIndex,
                                            SolverWorkBuffers& workBuffers,
                                            const std::vector<DistanceConstraint>& distanceConstraints,
                                            f32 dt,
                                            f32 contactCompliance,
                                            const std::function<f32(const RigidBodySoA&, u32)>& invMassFn);

/// Guarded pipeline dispatch with explicit wake/solve outcome.
IslandPipelineDispatchResult dispatch_solve_island_pipeline_result(
    RigidBodySoA& bodies,
    const ContactIslandGraph& graph,
    u32 islandIndex,
    SolverWorkBuffers& workBuffers,
    const std::vector<DistanceConstraint>& distanceConstraints,
    f32 dt,
    f32 contactCompliance,
    const std::function<f32(const RigidBodySoA&, u32)>& invMassFn);

/// Batch guarded pipeline dispatch (wake-then-solve); returns count of islands actually solved.
u32 dispatch_all_islands_pipeline_guarded(RigidBodySoA& bodies,
                                            const ContactIslandGraph& graph,
                                            SolverWorkBuffers& workBuffers,
                                            const std::vector<DistanceConstraint>& distanceConstraints,
                                            f32 dt,
                                            f32 contactCompliance,
                                            const std::function<f32(const RigidBodySoA&, u32)>& invMassFn);

/// Batch guarded pipeline dispatch with explicit wake/solve counts.
IslandPipelineBatchDispatchResult dispatch_all_islands_pipeline_result(
    RigidBodySoA& bodies,
    const ContactIslandGraph& graph,
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

/// Combined sleep + wake preflight for one island.
IslandSleepWakePreflight preflight_island_sleep_wake(const ContactIslandGraph::Island& island,
                                                     const RigidBodySoA& bodies);

/// Combined sleep + wake preflight by island index; out-of-range indices are marked skipped.
IslandSleepWakePreflight preflight_island_sleep_wake_by_index(const ContactIslandGraph& graph,
                                                              u32 islandIndex,
                                                              const RigidBodySoA& bodies);

/// Combined sleep + wake preflight for one island.
IslandSleepWakePreflight preflight_island_sleep_wake(const ContactIslandGraph::Island& island,
                                                     const RigidBodySoA& bodies);

/// Combined sleep + wake preflight by island index; out-of-range indices are marked skipped.
IslandSleepWakePreflight preflight_island_sleep_wake_by_index(const ContactIslandGraph& graph,
                                                              u32 islandIndex,
                                                              const RigidBodySoA& bodies);

/// Combined sleep + wake preflight for one island.
IslandSleepWakePreflight preflight_island_sleep_wake(const ContactIslandGraph::Island& island,
                                                     const RigidBodySoA& bodies);

/// Combined sleep + wake preflight by island index; out-of-range indices are marked skipped.
IslandSleepWakePreflight preflight_island_sleep_wake_by_index(const ContactIslandGraph& graph,
                                                              u32 islandIndex,
                                                              const RigidBodySoA& bodies);

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

/// Non-mutating sleep-solve skip predicate — mirrors `preflight_island_sleep`.
bool can_skip_island_sleep_solve(const ContactIslandGraph::Island& island, const RigidBodySoA& bodies);

/// Non-mutating sleep-solve predicate — inverse of `can_skip_island_sleep_solve`.
bool should_run_island_sleep_solve(const ContactIslandGraph::Island& island, const RigidBodySoA& bodies);

/// Summarize wakeable islands for batch guards.
IslandWakeGraphStats compute_island_wake_stats(const ContactIslandGraph& graph, const RigidBodySoA& bodies);

/// Graph-level wake preflight; sets `skipped` when no island needs sleeper activation.
IslandWakeGraphPreflight preflight_island_wake_graph(const ContactIslandGraph& graph,

/// Early-out guard for graph-level wake batching.
bool should_skip_island_wake_graph(const ContactIslandGraph& graph, const RigidBodySoA& bodies);

/// Early-out guard when an island does not need sleeper activation.
bool should_skip_island_wake(const ContactIslandGraph::Island& island, const RigidBodySoA& bodies);

/// Non-mutating wake predicate — inverse of `should_skip_island_wake` (B4.4 deepen follow-up pass).
/// Non-mutating wake predicate — inverse of `should_skip_island_wake`.
bool should_run_island_wake(const ContactIslandGraph::Island& island, const RigidBodySoA& bodies);

/// Non-mutating graph wake predicate — inverse of `should_skip_island_wake_graph`.
bool should_run_island_wake_graph(const ContactIslandGraph& graph, const RigidBodySoA& bodies);
/// Non-mutating wake skip predicate — mirrors `preflight_island_wake`.
bool can_skip_island_wake(const ContactIslandGraph::Island& island, const RigidBodySoA& bodies);

/// Non-mutating wake predicate — inverse of `can_skip_island_wake`.

/// Guarded wake-then-solve for one island; wakes sleepers when needed, then solves when allowed.
bool solve_island_with_wake_guarded(RigidBodySoA& bodies,
                                    const ContactIslandGraph::Island& island,
                                    SolverWorkBuffers& workBuffers,
                                    const std::vector<DistanceConstraint>& distanceConstraints,
                                    f32 dt,
                                    f32 contactCompliance,
                                    const std::function<f32(const RigidBodySoA&, u32)>& invMassFn);

/// Collect island indices that are not all-sleeping (parallel solve prep stub).
std::vector<u32> collect_nonsleeping_island_indices(const ContactIslandGraph& graph,

/// Collect island indices that should wake sleeping neighbors before solve.
std::vector<u32> collect_wakeable_island_indices(const ContactIslandGraph& graph,

/// Collect island indices that pass constraint-ref, body, and sleep solve guards.
std::vector<u32> collect_solveable_island_indices(
    const ContactIslandGraph& graph,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints);

/// Summarize solveable vs sleeping/stale islands for graph-level batch guards.
IslandSolveableStats compute_island_solveable_stats(
    const ContactIslandGraph& graph,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints);

/// Graph-level solveable preflight; sets `skipped` when no island can be solved.
IslandSolveableGraphPreflight preflight_island_solveable_graph(
    const ContactIslandGraph& graph,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints,
    f32 dt);

/// Early-out guard when every constrained island is blocked by sleep or stale refs.
bool should_skip_island_solveable_graph(const ContactIslandGraph& graph,
                                        const RigidBodySoA& bodies,
                                        const std::vector<narrowphase::ContactManifold>& contacts,
                                        const std::vector<DistanceConstraint>& distanceConstraints,
                                        f32 dt);

/// Guarded wake for sleeping bodies in one island; returns false when wake is unnecessary.
bool wake_island_sleepers_guarded(RigidBodySoA& bodies, const ContactIslandGraph::Island& island);

/// Guarded wake with explicit skip/activate outcome.
/// Guarded wake with explicit skip/wake outcome.
IslandWakeResult wake_island_sleepers_result(RigidBodySoA& bodies,
                                             const ContactIslandGraph& graph,
                                             u32 islandIndex);
/// Guarded wake with explicit preflight output (B4.4 deepen follow-up pass).
bool wake_island_sleepers_with_preflight(RigidBodySoA& bodies,
                                         const ContactIslandGraph::Island& island,
                                         IslandWakePreflight& outPreflight);

/// True when every constrained island is all-sleeping (graph-level batch skip predicate).
bool island_sleep_graph_rejects_all(const ContactIslandGraph& graph, const RigidBodySoA& bodies);

/// True when no island needs sleeper activation (graph-level batch skip predicate).
bool island_wake_graph_rejects_all(const ContactIslandGraph& graph, const RigidBodySoA& bodies);

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

/// Guarded wake with explicit skip/woke outcome for one island.
IslandWakeResult wake_island_sleepers_result(RigidBodySoA& bodies,
                                             const ContactIslandGraph::Island& island,
                                             u32 islandIndex = ContactIslandGraph::invalidIsland);

/// Guarded wake by island index with explicit skip/woke outcome.
IslandWakeResult wake_island_sleepers_by_index_result(RigidBodySoA& bodies,
                                                      const ContactIslandGraph& graph,
                                                      u32 islandIndex);

/// Batch guarded wake with explicit skip/woke counts.
IslandBatchWakeResult wake_all_island_sleepers_result(RigidBodySoA& bodies,
                                                      const ContactIslandGraph& graph);

/// Preflight island dispatch including sleep-state batch guards.
IslandSleepDispatchPreflight preflight_island_sleep_dispatch(const ContactIslandGraph& graph,
                                                             const RigidBodySoA& bodies,
                                                             f32 dt);

/// Early-out guard combining dispatch, timestep, and all-sleeping graph state.
bool should_skip_island_sleep_dispatch(const ContactIslandGraph& graph,
                                       const RigidBodySoA& bodies,
                                       f32 dt);

/// Batch guarded dispatch that wakes mixed islands then solves nonsleeping islands.
u32 dispatch_all_islands_with_wake_guarded(
    RigidBodySoA& bodies,
    const ContactIslandGraph& graph,
    SolverWorkBuffers& workBuffers,
    const std::vector<DistanceConstraint>& distanceConstraints,
    f32 dt,
    f32 contactCompliance,
    const std::function<f32(const RigidBodySoA&, u32)>& invMassFn);

/// Batch guarded dispatch with wake + explicit skip/solve counts.
IslandBatchDispatchResult dispatch_all_islands_with_wake_result(
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

/// Per-island wake outcome (skip vs activate) for parallel batch stubs.
struct IslandWakeResult {
    bool woke = false;
    bool skipped = false;
    u32 islandIndex = ContactIslandGraph::invalidIsland;
    u32 bodiesWoken = 0;
};

/// Batch wake summary for parallel iteration stubs.
struct IslandBatchWakeResult {
    u32 wokeCount = 0;
    u32 skippedCount = 0;
    u32 wakeableCount = 0;
    bool skipped = false;

    bool any_woke() const { return wokeCount > 0u; }
};

/// Combined sleep/wake + dispatch preflight for per-substep island iteration stubs.
struct IslandSleepWakeDispatchPreflight {
    IslandDispatchPreflight dispatch{};
    IslandSleepGraphPreflight sleep{};
    IslandWakeGraphPreflight wake{};
    bool skipped = false;

    bool can_dispatch() const { return !skipped && dispatch.can_dispatch() && sleep.has_solveable_islands(); }
};

/// Per-island sleep/wake + solve outcome for guarded dispatch stubs.
struct IslandSleepWakeDispatchResult {
    bool wokeSleepers = false;
    bool solved = false;
    bool skipped = false;
    u32 islandIndex = ContactIslandGraph::invalidIsland;
};

/// Batch sleep/wake + solve summary for parallel iteration stubs.
struct IslandBatchSleepWakeDispatchResult {
    u32 solvedCount = 0;
    u32 skippedCount = 0;
    u32 wokeCount = 0;
    u32 solveableCount = 0;
    bool skipped = false;

    bool any_solved() const { return solvedCount > 0u; }
};

/// Collect island indices that are dispatchable and not all-sleeping (parallel solve prep stub).
std::vector<u32> collect_solveable_island_indices(const ContactIslandGraph& graph,
                                                  const RigidBodySoA& bodies);

/// Count islands that pass nonsleeping + dispatchable guards.
u32 count_solveable_islands(const ContactIslandGraph& graph, const RigidBodySoA& bodies);

/// True when at least one island can be solved after sleep preflight.
bool has_solveable_islands(const ContactIslandGraph& graph, const RigidBodySoA& bodies);

/// Combined sleep/wake + dispatch preflight; sets `skipped` when nothing can solve.
IslandSleepWakeDispatchPreflight preflight_island_sleep_wake_dispatch(const ContactIslandGraph& graph,
                                                                      const RigidBodySoA& bodies,
                                                                      f32 dt);

/// Early-out guard combining dispatch, sleep, and timestep validity.
bool should_skip_island_sleep_wake_dispatch(const ContactIslandGraph& graph,
                                            const RigidBodySoA& bodies,
                                            f32 dt);

/// Guarded solve with full constraint-ref + body participation preflight.
bool solve_island_job_guarded(RigidBodySoA& bodies,
                              const ContactIslandGraph::Island& island,
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

/// Guarded dispatch: wake mixed islands, skip all-sleeping, then solve.
bool dispatch_solve_island_with_sleep_wake_guards(RigidBodySoA& bodies,
                                                  const ContactIslandGraph& graph,
                                                  u32 islandIndex,
                                                  SolverWorkBuffers& workBuffers,
                                                  const std::vector<DistanceConstraint>& distanceConstraints,
                                                  f32 dt,
                                                  f32 contactCompliance,
                                                  const std::function<f32(const RigidBodySoA&, u32)>& invMassFn);

/// Guarded dispatch with explicit wake/solve/skip outcome.
IslandSleepWakeDispatchResult dispatch_solve_island_with_sleep_wake_result(
    RigidBodySoA& bodies,
    const ContactIslandGraph& graph,
    u32 islandIndex,
    SolverWorkBuffers& workBuffers,
    const std::vector<DistanceConstraint>& distanceConstraints,
    f32 dt,
    f32 contactCompliance,
    const std::function<f32(const RigidBodySoA&, u32)>& invMassFn);

/// Batch guarded dispatch over solveable islands; returns count actually solved.
u32 dispatch_all_islands_with_sleep_wake_guards(RigidBodySoA& bodies,
                                                const ContactIslandGraph& graph,
                                                SolverWorkBuffers& workBuffers,
                                                const std::vector<DistanceConstraint>& distanceConstraints,
                                                f32 dt,
                                                f32 contactCompliance,
                                                const std::function<f32(const RigidBodySoA&, u32)>& invMassFn);

/// Batch guarded dispatch with explicit wake/solve/skip counts.
IslandBatchSleepWakeDispatchResult dispatch_all_islands_with_sleep_wake_result(
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

/// Batch guarded wake with explicit skip/woke counts.
IslandWakeBatchResult wake_all_island_sleepers_result(RigidBodySoA& bodies, const ContactIslandGraph& graph);

/// Preflight one extracted island job including timestep and constraint-solve coverage.
IslandFullSolvePreflight preflight_solve_island_job_full(
    const IslandSolveJob& job,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints,
    f32 dt);

/// Early-out guard combining job dispatch and constraint-solve preflights.
bool should_skip_solve_island_job_full(const IslandSolveJob& job,
                                       const RigidBodySoA& bodies,
                                       const std::vector<narrowphase::ContactManifold>& contacts,
                                       const std::vector<DistanceConstraint>& distanceConstraints,
                                       f32 dt);

/// Guarded island constraint solve; returns false when refs, bodies, or dt block solve.
bool solve_island_job_guarded(RigidBodySoA& bodies,
                              const ContactIslandGraph::Island& island,
                              SolverWorkBuffers& workBuffers,
                              const std::vector<DistanceConstraint>& distanceConstraints,
                              f32 dt,
                              f32 contactCompliance,
                              const std::function<f32(const RigidBodySoA&, u32)>& invMassFn);

/// Guarded island constraint solve with explicit skip/solve outcome.
IslandConstraintSolveResult solve_island_job_guarded_result(
    RigidBodySoA& bodies,
    const ContactIslandGraph& graph,
    u32 islandIndex,
    SolverWorkBuffers& workBuffers,
    const std::vector<DistanceConstraint>& distanceConstraints,
    f32 dt,
    f32 contactCompliance,
    const std::function<f32(const RigidBodySoA&, u32)>& invMassFn);

/// Preflight sleep + wake + constraint-solve for one island.
IslandSleepWakeSolvePreflight preflight_island_sleep_wake_solve(
    const ContactIslandGraph::Island& island,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints);

/// Preflight sleep + wake + constraint-solve by island index; out-of-range indices are marked skipped.
IslandSleepWakeSolvePreflight preflight_island_sleep_wake_solve_by_index(
    const ContactIslandGraph& graph,
    u32 islandIndex,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints);

/// Early-out guard when an island is all-sleeping or cannot solve after wake hints.
bool should_skip_island_sleep_wake_solve(const ContactIslandGraph::Island& island,
                                         const RigidBodySoA& bodies,
                                         const std::vector<narrowphase::ContactManifold>& contacts,
                                         const std::vector<DistanceConstraint>& distanceConstraints);

/// Summarize solveable vs all-sleeping islands for wake-then-solve batch guards.
IslandSleepWakeSolveGraphStats compute_island_sleep_wake_solve_stats(
    const ContactIslandGraph& graph,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints);

/// Graph-level sleep + wake + constraint-solve preflight; sets `skipped` when nothing can solve.
IslandSleepWakeSolveGraphPreflight preflight_island_sleep_wake_solve_graph(
    const ContactIslandGraph& graph,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints);

/// Early-out guard for graph-level wake-then-solve batching.
bool should_skip_island_sleep_wake_solve_graph(
    const ContactIslandGraph& graph,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints);

/// Collect island indices that are not all-sleeping and pass constraint-solve preflight.
std::vector<u32> collect_sleep_wake_solveable_island_indices(
    const ContactIslandGraph& graph,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints);

/// Guarded wake-then-solve for one island; wakes mixed sleepers before constraint solve.
bool wake_and_solve_island_guarded(RigidBodySoA& bodies,
                                   const ContactIslandGraph::Island& island,
                                   SolverWorkBuffers& workBuffers,
                                   const std::vector<DistanceConstraint>& distanceConstraints,
                                   f32 dt,
                                   f32 contactCompliance,
                                   const std::function<f32(const RigidBodySoA&, u32)>& invMassFn);

/// Guarded wake-then-solve by island index; returns false for out-of-range or skippable islands.
bool wake_and_solve_island_by_index_guarded(RigidBodySoA& bodies,
                                              const ContactIslandGraph& graph,
                                              u32 islandIndex,
                                              SolverWorkBuffers& workBuffers,
                                              const std::vector<DistanceConstraint>& distanceConstraints,
                                              f32 dt,
                                              f32 contactCompliance,
                                              const std::function<f32(const RigidBodySoA&, u32)>& invMassFn);

/// Batch guarded wake-then-solve over solveable islands; returns count actually solved.
u32 dispatch_all_islands_sleep_wake_guarded(
    RigidBodySoA& bodies,
    const ContactIslandGraph& graph,
    SolverWorkBuffers& workBuffers,
    const std::vector<DistanceConstraint>& distanceConstraints,
    f32 dt,
    f32 contactCompliance,
    const std::function<f32(const RigidBodySoA&, u32)>& invMassFn);

/// Batch guarded wake-then-solve with explicit skip/solve/wake counts.
IslandSleepWakeSolveBatchResult dispatch_all_islands_sleep_wake_result(
    RigidBodySoA& bodies,
    const ContactIslandGraph& graph,
    SolverWorkBuffers& workBuffers,
    const std::vector<DistanceConstraint>& distanceConstraints,
    f32 dt,
    f32 contactCompliance,
    const std::function<f32(const RigidBodySoA&, u32)>& invMassFn);

/// Preflight dispatch combined with sleep graph stats for sleep-aware batching.
IslandSleepAwareDispatchPreflight preflight_island_sleep_aware_dispatch(const ContactIslandGraph& graph,
                                                                         const RigidBodySoA& bodies,
                                                                         f32 dt);

/// Early-out guard when dispatch is blocked by invalid dt or all islands are all-sleeping.
bool should_skip_island_sleep_aware_dispatch(const ContactIslandGraph& graph,
                                             const RigidBodySoA& bodies,
                                             f32 dt);

/// Collect dispatchable island indices that are not all-sleeping.
std::vector<u32> collect_sleep_aware_dispatchable_island_indices(const ContactIslandGraph& graph,
                                                                 const RigidBodySoA& bodies);

/// Guarded dispatch that skips all-sleeping islands (does not wake sleepers).
bool dispatch_solve_island_sleep_aware_guarded(RigidBodySoA& bodies,
                                               const ContactIslandGraph& graph,
                                               u32 islandIndex,
                                               SolverWorkBuffers& workBuffers,
                                               const std::vector<DistanceConstraint>& distanceConstraints,
                                               f32 dt,
                                               f32 contactCompliance,
                                               const std::function<f32(const RigidBodySoA&, u32)>& invMassFn);

/// Wake mixed-island sleepers then solve when constraint preflight allows.
bool wake_and_solve_island_guarded(RigidBodySoA& bodies,
                                   const ContactIslandGraph& graph,
                                   u32 islandIndex,
                                   SolverWorkBuffers& workBuffers,
                                   const std::vector<DistanceConstraint>& distanceConstraints,
                                   f32 dt,
                                   f32 contactCompliance,
                                   const std::function<f32(const RigidBodySoA&, u32)>& invMassFn);

/// Batch wake-then-solve over wakeable, constraint-solveable islands.
u32 wake_and_dispatch_all_islands_guarded(
    RigidBodySoA& bodies,
    const ContactIslandGraph& graph,
    SolverWorkBuffers& workBuffers,
    const std::vector<DistanceConstraint>& distanceConstraints,
    f32 dt,
    f32 contactCompliance,
    const std::function<f32(const RigidBodySoA&, u32)>& invMassFn);

/// Guarded wake with explicit skip/activate outcome.
IslandWakeResult wake_island_sleepers_result(RigidBodySoA& bodies,
                                             const ContactIslandGraph::Island& island,
                                             u32 islandIndex = ContactIslandGraph::invalidIsland);

/// Guarded wake by island index with explicit skip/activate outcome.
IslandWakeResult wake_island_sleepers_by_index_result(RigidBodySoA& bodies,
                                                      const ContactIslandGraph& graph,
                                                      u32 islandIndex);

/// Batch guarded wake with explicit skip/activate counts.
IslandBatchWakeResult wake_all_island_sleepers_result(RigidBodySoA& bodies,
                                                      const ContactIslandGraph& graph);

/// Preflight wake-then-dispatch batch for solve-after-wake stubs.
IslandDispatchAfterWakePreflight preflight_island_dispatch_after_wake(const ContactIslandGraph& graph,
                                                                      const RigidBodySoA& bodies,
                                                                      f32 dt);

/// Early-out guard for wake-then-dispatch when nothing is dispatchable.
bool should_skip_island_dispatch_after_wake(const ContactIslandGraph& graph,
                                            const RigidBodySoA& bodies,
                                            f32 dt);

/// Guarded wake-then-solve for one island; wakes mixed-sleep islands before solve.
bool dispatch_solve_island_after_wake_guarded(RigidBodySoA& bodies,
                                              const ContactIslandGraph& graph,
                                              u32 islandIndex,
                                              SolverWorkBuffers& workBuffers,
                                              const std::vector<DistanceConstraint>& distanceConstraints,
                                              f32 dt,
                                              f32 contactCompliance,
                                              const std::function<f32(const RigidBodySoA&, u32)>& invMassFn);

/// Guarded wake-then-solve with explicit skip/solve outcome.
IslandDispatchResult dispatch_solve_island_after_wake_result(
    RigidBodySoA& bodies,
    const ContactIslandGraph& graph,
    u32 islandIndex,
    SolverWorkBuffers& workBuffers,
    const std::vector<DistanceConstraint>& distanceConstraints,
    f32 dt,
    f32 contactCompliance,
    const std::function<f32(const RigidBodySoA&, u32)>& invMassFn);

/// Batch guarded wake-then-solve over solveable islands; returns count solved.
u32 dispatch_all_islands_after_wake_guarded(
    RigidBodySoA& bodies,
    const ContactIslandGraph& graph,
    SolverWorkBuffers& workBuffers,
    const std::vector<DistanceConstraint>& distanceConstraints,
    f32 dt,
    f32 contactCompliance,
    const std::function<f32(const RigidBodySoA&, u32)>& invMassFn);

/// Batch guarded wake-then-solve with explicit skip/solve counts.
IslandBatchDispatchResult dispatch_all_islands_after_wake_result(
    RigidBodySoA& bodies,
    const ContactIslandGraph& graph,
    SolverWorkBuffers& workBuffers,
    const std::vector<DistanceConstraint>& distanceConstraints,
    f32 dt,
    f32 contactCompliance,
    const std::function<f32(const RigidBodySoA&, u32)>& invMassFn);

/// Guarded wake with explicit skip/activated outcome.
IslandWakeResult wake_island_sleepers_result(RigidBodySoA& bodies, const ContactIslandGraph::Island& island);

/// Guarded wake by island index with explicit outcome.
IslandWakeResult wake_island_sleepers_by_index_result(RigidBodySoA& bodies,
                                                      const ContactIslandGraph& graph,
                                                      u32 islandIndex);

/// Batch guarded wake with explicit skip/activated counts.
IslandBatchWakeResult wake_all_island_sleepers_result(RigidBodySoA& bodies, const ContactIslandGraph& graph);

/// Preflight one extracted island job including constraint participation and sleep state.
IslandJobSolvePreflight preflight_island_job_solve(const IslandSolveJob& job,
                                                   const RigidBodySoA& bodies,
                                                   const std::vector<narrowphase::ContactManifold>& contacts,
                                                   const std::vector<DistanceConstraint>& distanceConstraints,
                                                   f32 dt);

/// Early-out guard when job dispatch, constraint refs/bodies, or sleep block island solve.
bool should_skip_island_job_solve(const IslandSolveJob& job,
                                  const RigidBodySoA& bodies,
                                  const std::vector<narrowphase::ContactManifold>& contacts,
                                  const std::vector<DistanceConstraint>& distanceConstraints,
                                  f32 dt);

/// Summarize solveable vs skipped islands for composed batch guards.
IslandGraphSolveStats compute_island_graph_solve_stats(
    const ContactIslandGraph& graph,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints,
    f32 dt);

/// Count islands that pass composed job-solve preflight.
u32 count_solveable_islands(const ContactIslandGraph& graph,
                            const RigidBodySoA& bodies,
                            const std::vector<narrowphase::ContactManifold>& contacts,
                            const std::vector<DistanceConstraint>& distanceConstraints,
                            f32 dt);

/// True when at least one island can be solved this substep.
bool has_solveable_islands(const ContactIslandGraph& graph,
                           const RigidBodySoA& bodies,
                           const std::vector<narrowphase::ContactManifold>& contacts,
                           const std::vector<DistanceConstraint>& distanceConstraints,
                           f32 dt);

/// Graph-level composed solve preflight; sets `skipped` when nothing is solveable.
IslandGraphSolvePreflight preflight_island_graph_solve(const ContactIslandGraph& graph,
                                                       const RigidBodySoA& bodies,
                                                       const std::vector<narrowphase::ContactManifold>& contacts,
                                                       const std::vector<DistanceConstraint>& distanceConstraints,
                                                       f32 dt);

/// Early-out guard for graph-level composed solve batching.
bool should_skip_island_graph_solve(const ContactIslandGraph& graph,
                                    const RigidBodySoA& bodies,
                                    const std::vector<narrowphase::ContactManifold>& contacts,
                                    const std::vector<DistanceConstraint>& distanceConstraints,
                                    f32 dt);

/// Collect island indices that pass composed job-solve preflight.
std::vector<u32> collect_solveable_island_indices(
    const ContactIslandGraph& graph,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints,
    f32 dt);

/// Keep only jobs that pass composed job-solve preflight.
std::vector<IslandSolveJob> filter_solveable_jobs(const std::vector<IslandSolveJob>& jobs,
                                                  const RigidBodySoA& bodies,
                                                  const std::vector<narrowphase::ContactManifold>& contacts,
                                                  const std::vector<DistanceConstraint>& distanceConstraints,
                                                  f32 dt);

/// Guarded dispatch with composed preflight; skips empty, sleeping, and stale-ref islands.
bool dispatch_solve_island_guarded(RigidBodySoA& bodies,
                                   const ContactIslandGraph& graph,
                                   u32 islandIndex,
                                   SolverWorkBuffers& workBuffers,
                                   const std::vector<DistanceConstraint>& distanceConstraints,
                                   f32 dt,
                                   f32 contactCompliance,
                                   const std::function<f32(const RigidBodySoA&, u32)>& invMassFn);

/// Guarded dispatch from a pre-extracted job with composed preflight.
bool dispatch_solve_island_job_guarded(RigidBodySoA& bodies,
                                       const IslandSolveJob& job,
                                       SolverWorkBuffers& workBuffers,
                                       const std::vector<DistanceConstraint>& distanceConstraints,
                                       f32 dt,
                                       f32 contactCompliance,
                                       const std::function<f32(const RigidBodySoA&, u32)>& invMassFn);

/// Guarded dispatch with explicit skip/solve outcome and composed preflight.
IslandDispatchResult dispatch_solve_island_guarded_result(RigidBodySoA& bodies,
                                                          const ContactIslandGraph& graph,
                                                          u32 islandIndex,
                                                          SolverWorkBuffers& workBuffers,
                                                          const std::vector<DistanceConstraint>& distanceConstraints,
                                                          f32 dt,
                                                          f32 contactCompliance,
                                                          const std::function<f32(const RigidBodySoA&, u32)>& invMassFn);

/// Batch guarded dispatch over solveable islands only.
u32 dispatch_all_solveable_islands(RigidBodySoA& bodies,
                                   const ContactIslandGraph& graph,
                                   SolverWorkBuffers& workBuffers,
                                   const std::vector<DistanceConstraint>& distanceConstraints,
                                   f32 dt,
                                   f32 contactCompliance,
                                   const std::function<f32(const RigidBodySoA&, u32)>& invMassFn);

/// Batch guarded dispatch with explicit skip/solve counts for solveable islands.
IslandBatchSolveResult dispatch_all_solveable_islands_result(
    RigidBodySoA& bodies,
    const ContactIslandGraph& graph,
    SolverWorkBuffers& workBuffers,
    const std::vector<DistanceConstraint>& distanceConstraints,
    f32 dt,
    f32 contactCompliance,
    const std::function<f32(const RigidBodySoA&, u32)>& invMassFn);

/// Wake sleepers then guarded-dispatch one island; returns false when solve is skipped.
bool dispatch_solve_island_with_wake_guarded(RigidBodySoA& bodies,
                                             const ContactIslandGraph& graph,
                                             u32 islandIndex,
                                             SolverWorkBuffers& workBuffers,
                                             const std::vector<DistanceConstraint>& distanceConstraints,
                                             f32 dt,
                                             f32 contactCompliance,
                                             const std::function<f32(const RigidBodySoA&, u32)>& invMassFn);

/// Batch wake-then-solve over solveable islands with explicit counts.
IslandBatchSolveResult dispatch_all_islands_with_wake_result(
    RigidBodySoA& bodies,
    const ContactIslandGraph& graph,
    SolverWorkBuffers& workBuffers,
    const std::vector<DistanceConstraint>& distanceConstraints,
    f32 dt,
    f32 contactCompliance,
    const std::function<f32(const RigidBodySoA&, u32)>& invMassFn);

/// Batch guarded wake with explicit skip/woke counts.
IslandBatchWakeResult wake_all_island_sleepers_result(RigidBodySoA& bodies,
                                                      const ContactIslandGraph& graph);

/// Guarded wake with explicit skip/activate outcome.
IslandWakeResult wake_island_sleepers_result(RigidBodySoA& bodies,
                                             const ContactIslandGraph& graph,
                                             u32 islandIndex);

/// Batch guarded wake with explicit skip/activate counts.
IslandBatchWakeResult wake_all_island_sleepers_result(RigidBodySoA& bodies,
                                                      const ContactIslandGraph& graph);

/// Preflight sleep + dispatch for graph-level solve batching.
IslandSleepDispatchPreflight preflight_island_sleep_dispatch(const ContactIslandGraph& graph,
                                                             const RigidBodySoA& bodies,
                                                             f32 dt);

/// Early-out guard combining sleep graph and dispatch preflight.
bool should_skip_island_sleep_dispatch(const ContactIslandGraph& graph,
                                       const RigidBodySoA& bodies,
                                       f32 dt);

/// Batch guarded dispatch skipping all-sleeping islands; returns count actually solved.
u32 dispatch_all_islands_skipping_sleepers(RigidBodySoA& bodies,
                                           const ContactIslandGraph& graph,
                                           SolverWorkBuffers& workBuffers,
                                           const std::vector<DistanceConstraint>& distanceConstraints,
                                           f32 dt,
                                           f32 contactCompliance,
                                           const std::function<f32(const RigidBodySoA&, u32)>& invMassFn);

/// Batch guarded dispatch skipping all-sleeping islands with explicit counts.
IslandBatchSleepSkipDispatchResult dispatch_all_islands_skipping_sleepers_result(
    RigidBodySoA& bodies,
    const ContactIslandGraph& graph,
    SolverWorkBuffers& workBuffers,
    const std::vector<DistanceConstraint>& distanceConstraints,
    f32 dt,
    f32 contactCompliance,
    const std::function<f32(const RigidBodySoA&, u32)>& invMassFn);

/// Batch guarded dispatch that wakes mixed islands before solving.
IslandBatchSleepSkipDispatchResult dispatch_all_islands_with_wake_result(
    RigidBodySoA& bodies,
    const ContactIslandGraph& graph,
    SolverWorkBuffers& workBuffers,
    const std::vector<DistanceConstraint>& distanceConstraints,
    f32 dt,
    f32 contactCompliance,
    const std::function<f32(const RigidBodySoA&, u32)>& invMassFn);

/// Per-island constraint solve outcome with reject reason (B4.4 deepen follow-up pass).
struct IslandConstraintSolveResult {
    bool solved = false;
    bool skipped = false;
    IslandConstraintSolveRejectReason reason = IslandConstraintSolveRejectReason::None;
};

/// Guarded island constraint solve using full constraint-ref + body participation preflight.
bool solve_island_job_guarded(RigidBodySoA& bodies,
                              const ContactIslandGraph::Island& island,
                              SolverWorkBuffers& workBuffers,
                              const std::vector<DistanceConstraint>& distanceConstraints,
                              f32 dt,
                              f32 contactCompliance,
                              const std::function<f32(const RigidBodySoA&, u32)>& invMassFn);

/// Guarded island constraint solve with explicit skip/solve outcome and reject reason.
IslandConstraintSolveResult solve_island_job_guarded_result(
    RigidBodySoA& bodies,
    const ContactIslandGraph::Island& island,
    SolverWorkBuffers& workBuffers,
    const std::vector<DistanceConstraint>& distanceConstraints,
    f32 dt,
    f32 contactCompliance,
    const std::function<f32(const RigidBodySoA&, u32)>& invMassFn);

/// Wake sleepers when needed, then run guarded constraint solve for one island.
IslandConstraintSolveResult solve_island_job_with_wake_guarded(
    RigidBodySoA& bodies,
    const ContactIslandGraph::Island& island,
    SolverWorkBuffers& workBuffers,
    const std::vector<DistanceConstraint>& distanceConstraints,
    f32 dt,
    f32 contactCompliance,
    const std::function<f32(const RigidBodySoA&, u32)>& invMassFn);

/// Guarded dispatch from a pre-extracted job with constraint-solve preflight.
IslandDispatchResult dispatch_solve_island_job_guarded(RigidBodySoA& bodies,
                                                     const IslandSolveJob& job,
                                                     SolverWorkBuffers& workBuffers,
                                                     const std::vector<DistanceConstraint>& distanceConstraints,
                                                     f32 dt,
                                                     f32 contactCompliance,
                                                     const std::function<f32(const RigidBodySoA&, u32)>& invMassFn);

/// Guarded island solve pipeline: wake sleepers, then dispatch when deepen preflight allows.
bool dispatch_solve_island_pipeline_guarded(RigidBodySoA& bodies,
                                            const ContactIslandGraph& graph,
                                            u32 islandIndex,
                                            SolverWorkBuffers& workBuffers,
                                            const std::vector<DistanceConstraint>& distanceConstraints,
                                            f32 dt,
                                            f32 contactCompliance,
                                            const std::function<f32(const RigidBodySoA&, u32)>& invMassFn);

/// Guarded island solve pipeline with explicit skip/solve outcome.
IslandDispatchResult dispatch_solve_island_pipeline_result(
    RigidBodySoA& bodies,
    const ContactIslandGraph& graph,
    u32 islandIndex,
    SolverWorkBuffers& workBuffers,
    const std::vector<DistanceConstraint>& distanceConstraints,
    f32 dt,
    f32 contactCompliance,
    const std::function<f32(const RigidBodySoA&, u32)>& invMassFn);

/// Batch guarded pipeline dispatch over nonsleeping islands; returns count actually solved.
u32 dispatch_all_islands_pipeline_guarded(RigidBodySoA& bodies,
                                            const ContactIslandGraph& graph,
                                            SolverWorkBuffers& workBuffers,
                                            const std::vector<DistanceConstraint>& distanceConstraints,
                                            f32 dt,
                                            f32 contactCompliance,
                                            const std::function<f32(const RigidBodySoA&, u32)>& invMassFn);

/// Batch guarded pipeline dispatch with explicit skip/solve counts.
IslandBatchDispatchResult dispatch_all_islands_pipeline_result(
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

/// Preflight pipeline solve for one island (job + sleep + constraint guards).
IslandPipelineSolvePreflight preflight_pipeline_solve_island(
    const ContactIslandGraph::Island& island,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints,
    f32 dt);

/// Preflight pipeline solve by island index; out-of-range indices are marked skipped.
IslandPipelineSolvePreflight preflight_pipeline_solve_island_by_index(
    const ContactIslandGraph& graph,
    u32 islandIndex,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints,
    f32 dt);

/// Early-out guard when pipeline solve preflight blocks island work.
bool should_skip_pipeline_solve_island(const ContactIslandGraph::Island& island,
                                       const RigidBodySoA& bodies,
                                       const std::vector<narrowphase::ContactManifold>& contacts,
                                       const std::vector<DistanceConstraint>& distanceConstraints,
                                       f32 dt);

/// Summarize pipeline-dispatchable vs sleeping/stale islands for batch guards.
IslandPipelineDispatchStats compute_island_pipeline_dispatch_stats(
    const ContactIslandGraph& graph,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints,
    f32 dt);

/// Count islands that pass pipeline solve preflight.
u32 count_pipeline_dispatchable_islands(const ContactIslandGraph& graph,
                                        const RigidBodySoA& bodies,
                                        const std::vector<narrowphase::ContactManifold>& contacts,
                                        const std::vector<DistanceConstraint>& distanceConstraints,
                                        f32 dt);

/// True when at least one island would be pipeline-dispatched this substep.
bool has_pipeline_dispatchable_islands(const ContactIslandGraph& graph,
                                       const RigidBodySoA& bodies,
                                       const std::vector<narrowphase::ContactManifold>& contacts,
                                       const std::vector<DistanceConstraint>& distanceConstraints,
                                       f32 dt);

/// Graph-level pipeline dispatch preflight; sets `skipped` when nothing needs work.
IslandPipelineDispatchPreflight preflight_pipeline_dispatch(
    const ContactIslandGraph& graph,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints,
    f32 dt);

/// Early-out guard for pipeline dispatch (invalid dt or no solveable islands).
bool should_skip_pipeline_dispatch(const ContactIslandGraph& graph,
                                   const RigidBodySoA& bodies,
                                   const std::vector<narrowphase::ContactManifold>& contacts,
                                   const std::vector<DistanceConstraint>& distanceConstraints,
                                   f32 dt);

/// Collect island indices that pass pipeline solve preflight.
std::vector<u32> collect_pipeline_dispatchable_island_indices(
    const ContactIslandGraph& graph,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints,
    f32 dt);

/// Guarded pipeline dispatch: wake mixed islands, then solve when preflight allows.
bool dispatch_solve_island_pipeline_guarded(RigidBodySoA& bodies,
                                            const ContactIslandGraph& graph,
                                            u32 islandIndex,
                                            SolverWorkBuffers& workBuffers,
                                            const std::vector<DistanceConstraint>& distanceConstraints,
                                            f32 dt,
                                            f32 contactCompliance,
                                            const std::function<f32(const RigidBodySoA&, u32)>& invMassFn);

/// Guarded pipeline dispatch with explicit wake/skip/solve outcome.
IslandPipelineDispatchResult dispatch_solve_island_pipeline_result(
    RigidBodySoA& bodies,
    const ContactIslandGraph& graph,
    u32 islandIndex,
    SolverWorkBuffers& workBuffers,
    const std::vector<DistanceConstraint>& distanceConstraints,
    f32 dt,
    f32 contactCompliance,
    const std::function<f32(const RigidBodySoA&, u32)>& invMassFn);

/// Batch guarded pipeline dispatch over pipeline-dispatchable islands.
u32 dispatch_all_islands_pipeline_guarded(RigidBodySoA& bodies,
                                          const ContactIslandGraph& graph,
                                          SolverWorkBuffers& workBuffers,
                                          const std::vector<DistanceConstraint>& distanceConstraints,
                                          f32 dt,
                                          f32 contactCompliance,
                                          const std::function<f32(const RigidBodySoA&, u32)>& invMassFn);

/// Batch guarded pipeline dispatch with explicit wake/skip/solve counts.
IslandPipelineBatchDispatchResult dispatch_all_islands_pipeline_result(
    RigidBodySoA& bodies,
    const ContactIslandGraph& graph,
    SolverWorkBuffers& workBuffers,
    const std::vector<DistanceConstraint>& distanceConstraints,
    f32 dt,
    f32 contactCompliance,
    const std::function<f32(const RigidBodySoA&, u32)>& invMassFn);

/// Per-island pipeline dispatch preflight (wake + sleep + constraint-solve + dt guards).
struct IslandPipelineDispatchPreflight {
    IslandSolveJobPreflight job{};
    IslandSleepPreflight sleep{};
    IslandWakePreflight wake{};
    IslandConstraintSolvePreflight constraintSolve{};
    bool skipped = false;

    bool can_dispatch() const {
        return !skipped && job.can_dispatch() && !sleep.can_skip_solve() && constraintSolve.can_solve();
    }

    bool should_wake_first() const { return !skipped && wake.should_wake_sleepers(); }
};

/// Graph-level pipeline dispatch preflight for batch iteration stubs.
struct IslandPipelineGraphPreflight {
    IslandDispatchPreflight dispatch{};
    IslandSleepGraphPreflight sleep{};
    IslandWakeGraphPreflight wake{};
    bool skipped = false;

    bool can_dispatch() const { return !skipped && dispatch.can_dispatch() && sleep.has_solveable_islands(); }
};

/// Preflight one island for pipeline dispatch (sleep/wake/constraint guards + dt).
IslandPipelineDispatchPreflight preflight_island_pipeline_dispatch(
    const ContactIslandGraph& graph,
    u32 islandIndex,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints,
    f32 dt);

/// Preflight one extracted job for pipeline dispatch.
IslandPipelineDispatchPreflight preflight_island_pipeline_dispatch_job(
    const IslandSolveJob& job,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints,
    f32 dt);

/// Graph-level pipeline dispatch preflight; sets `skipped` when nothing is dispatchable.
IslandPipelineGraphPreflight preflight_island_pipeline_dispatch_graph(const ContactIslandGraph& graph,
                                                                      const RigidBodySoA& bodies,
                                                                      f32 dt);

/// Early-out guard for per-island pipeline dispatch.
bool should_skip_island_pipeline_dispatch(const ContactIslandGraph& graph,
                                          u32 islandIndex,
                                          const RigidBodySoA& bodies,
                                          const std::vector<narrowphase::ContactManifold>& contacts,
                                          const std::vector<DistanceConstraint>& distanceConstraints,
                                          f32 dt);

/// Early-out guard for graph-level pipeline dispatch batching.
bool should_skip_island_pipeline_dispatch_graph(const ContactIslandGraph& graph,
                                                const RigidBodySoA& bodies,
                                                f32 dt);

/// Collect island indices that pass pipeline dispatch preflight.
std::vector<u32> collect_pipeline_dispatchable_island_indices(
    const ContactIslandGraph& graph,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints,
    f32 dt);

/// Guarded pipeline dispatch: optional wake, then solve with all preflight guards.
bool dispatch_solve_island_pipeline_guarded(RigidBodySoA& bodies,
                                            const ContactIslandGraph& graph,
                                            u32 islandIndex,
                                            SolverWorkBuffers& workBuffers,
                                            const std::vector<DistanceConstraint>& distanceConstraints,
                                            f32 dt,
                                            f32 contactCompliance,
                                            const std::function<f32(const RigidBodySoA&, u32)>& invMassFn);

/// Guarded pipeline dispatch with explicit skip/solve outcome.
IslandDispatchResult dispatch_solve_island_pipeline_result(RigidBodySoA& bodies,
                                                           const ContactIslandGraph& graph,
                                                           u32 islandIndex,
                                                           SolverWorkBuffers& workBuffers,
                                                           const std::vector<DistanceConstraint>& distanceConstraints,
                                                           f32 dt,
                                                           f32 contactCompliance,
                                                           const std::function<f32(const RigidBodySoA&, u32)>& invMassFn);

/// Batch guarded pipeline dispatch over pipeline-dispatchable islands.
u32 dispatch_all_islands_pipeline_guarded(RigidBodySoA& bodies,
                                          const ContactIslandGraph& graph,
                                          SolverWorkBuffers& workBuffers,
                                          const std::vector<DistanceConstraint>& distanceConstraints,
                                          f32 dt,
                                          f32 contactCompliance,
                                          const std::function<f32(const RigidBodySoA&, u32)>& invMassFn);

/// Batch guarded pipeline dispatch with explicit skip/solve counts.
IslandBatchDispatchResult dispatch_all_islands_pipeline_result(
    RigidBodySoA& bodies,
    const ContactIslandGraph& graph,
    SolverWorkBuffers& workBuffers,
    const std::vector<DistanceConstraint>& distanceConstraints,
    f32 dt,
    f32 contactCompliance,
    const std::function<f32(const RigidBodySoA&, u32)>& invMassFn);

/// Returns true when `island_dispatch_reject_reason` matches `expected` (B4.4 deepen follow-up).
bool island_dispatch_rejects_for_reason(const ContactIslandGraph& graph,
                                        const RigidBodySoA& bodies,
                                        f32 dt,
                                        IslandDispatchRejectReason expected);

/// Returns true when `island_solve_reject_reason` matches `expected` (B4.4 deepen follow-up).
bool island_solve_rejects_for_reason(const ContactIslandGraph::Island& island,
                                     const RigidBodySoA& bodies,
                                     const std::vector<narrowphase::ContactManifold>& contacts,
                                     const std::vector<DistanceConstraint>& distanceConstraints,
                                     f32 dt,
                                     IslandSolveRejectReason expected);

/// Diagnose why island batch dispatch would early-out (B4.4 deepen follow-up).
IslandDispatchRejectReason island_dispatch_reject_reason(const ContactIslandGraph& graph,
                                                           const RigidBodySoA& bodies,
                                                           f32 dt);

/// Diagnose why one island constraint solve would early-out (B4.4 deepen follow-up).
IslandSolveRejectReason island_solve_reject_reason(
    const ContactIslandGraph::Island& island,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints,
    f32 dt);

/// Pipeline preflight chaining build, wake, sleep, and dispatch guards (B4.4 deepen follow-up).
IslandSolvePipelinePreflight preflight_island_solve_pipeline(
    u32 bodyCount,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints,
    const ContactIslandGraph& graph,
    const RigidBodySoA& bodies,
    f32 dt);

/// Early-out guard for the full island solve pipeline (B4.4 deepen follow-up).
bool should_skip_island_solve_pipeline(u32 bodyCount,
                                       const std::vector<narrowphase::ContactManifold>& contacts,
                                       const std::vector<DistanceConstraint>& distanceConstraints,
                                       const ContactIslandGraph& graph,
                                       const RigidBodySoA& bodies,
                                       f32 dt);

/// Combined constraint-ref + body participation preflight by island index (B4.4 deepen follow-up).
IslandConstraintSolvePreflight preflight_island_constraint_solve_by_index(
    const ContactIslandGraph& graph,
    u32 islandIndex,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints);

/// Early-out guard for constraint solve by island index (B4.4 deepen follow-up).
bool should_skip_island_constraint_solve_index(const ContactIslandGraph& graph,
                                               u32 islandIndex,
                                               const RigidBodySoA& bodies,
                                               const std::vector<narrowphase::ContactManifold>& contacts,
                                               const std::vector<DistanceConstraint>& distanceConstraints);

/// Collect island indices that are dispatchable and not all-sleeping (B4.4 deepen follow-up).
std::vector<u32> collect_solveable_island_indices(const ContactIslandGraph& graph,
                                                  const RigidBodySoA& bodies);

/// Guarded island solve using constraint-ref + body participation preflight (B4.4 deepen follow-up).
bool solve_island_job_guarded(RigidBodySoA& bodies,
                              const ContactIslandGraph::Island& island,
                              SolverWorkBuffers& workBuffers,
                              const std::vector<DistanceConstraint>& distanceConstraints,
                              f32 dt,
                              f32 contactCompliance,
                              const std::function<f32(const RigidBodySoA&, u32)>& invMassFn);

/// Guarded wake with explicit skip/woke outcome (B4.4 deepen follow-up).
IslandWakeResult wake_island_sleepers_result(RigidBodySoA& bodies,
                                             const ContactIslandGraph& graph,
                                             u32 islandIndex);

/// Batch guarded wake with explicit skip/woke counts (B4.4 deepen follow-up).
IslandBatchWakeResult wake_all_island_sleepers_result(RigidBodySoA& bodies,
                                                      const ContactIslandGraph& graph);

/// Guarded dispatch that wakes sleepers before solving one island (B4.4 deepen follow-up).
bool dispatch_solve_island_with_wake_guarded(RigidBodySoA& bodies,
                                             const ContactIslandGraph& graph,
                                             u32 islandIndex,
                                             SolverWorkBuffers& workBuffers,
                                             const std::vector<DistanceConstraint>& distanceConstraints,
                                             f32 dt,
                                             f32 contactCompliance,
                                             const std::function<f32(const RigidBodySoA&, u32)>& invMassFn);

/// Batch guarded dispatch over solveable (nonsleeping, dispatchable) islands (B4.4 deepen follow-up).
u32 dispatch_solveable_islands(RigidBodySoA& bodies,
                               const ContactIslandGraph& graph,
                               SolverWorkBuffers& workBuffers,
                               const std::vector<DistanceConstraint>& distanceConstraints,
                               f32 dt,
                               f32 contactCompliance,
                               const std::function<f32(const RigidBodySoA&, u32)>& invMassFn);

/// Batch guarded dispatch with explicit skip/solve counts for solveable islands (B4.4 deepen follow-up).
IslandBatchDispatchResult dispatch_solveable_islands_result(
    RigidBodySoA& bodies,
    const ContactIslandGraph& graph,
    SolverWorkBuffers& workBuffers,
    const std::vector<DistanceConstraint>& distanceConstraints,
    f32 dt,
    f32 contactCompliance,
    const std::function<f32(const RigidBodySoA&, u32)>& invMassFn);

/// Guarded pipeline: wake wakeable islands then dispatch solveable islands (B4.4 deepen follow-up).
IslandBatchDispatchResult dispatch_island_solve_pipeline_guarded(
    RigidBodySoA& bodies,
    const ContactIslandGraph& graph,
    SolverWorkBuffers& workBuffers,
    const std::vector<DistanceConstraint>& distanceConstraints,
    f32 dt,
    f32 contactCompliance,
    const std::function<f32(const RigidBodySoA&, u32)>& invMassFn);

/// Per-island pipeline dispatch outcome (wake → constraint-solve guards → solve).
struct IslandPipelineDispatchResult {
    u32 islandIndex = ContactIslandGraph::invalidIsland;
    bool wokeSleepers = false;
    bool solved = false;
    bool skipped = false;
    IslandWakeRejectReason wakeReason = IslandWakeRejectReason::None;
    IslandConstraintSolveRejectReason constraintReason = IslandConstraintSolveRejectReason::None;
    IslandSolveJobRejectReason dispatchReason = IslandSolveJobRejectReason::None;
};

/// Batch pipeline dispatch summary for wake → solve guarded iteration.
struct IslandBatchPipelineDispatchResult {
    u32 solvedCount = 0;
    u32 skippedCount = 0;
    u32 wokeCount = 0;
    u32 dispatchableCount = 0;
    bool skipped = false;

    bool any_solved() const { return solvedCount > 0u; }
};

/// Read-only pipeline dispatch preflight (wake graph + dispatch graph + dt guards).
struct IslandPipelineDispatchPreflight {
    IslandDispatchRejectReason dispatchReason = IslandDispatchRejectReason::None;
    IslandWakeGraphPreflight wake{};
    IslandDispatchPreflight solve{};
    bool skipped = false;

    bool can_dispatch() const { return !skipped && dispatchReason == IslandDispatchRejectReason::None; }
};

/// Populate pipeline dispatch preflight without mutating bodies or graph.
IslandPipelineDispatchPreflight preflight_island_pipeline_dispatch(const ContactIslandGraph& graph,
                                                                   const RigidBodySoA& bodies,
                                                                   f32 dt);

/// Early-out guard when pipeline dispatch would skip every island.
bool should_skip_island_pipeline_dispatch(const ContactIslandGraph& graph,
                                          const RigidBodySoA& bodies,
                                          f32 dt);

/// Guarded per-island pipeline: wake sleepers when needed, then dispatch constraint solve.
IslandPipelineDispatchResult dispatch_solve_island_pipeline_result(
    RigidBodySoA& bodies,
    const ContactIslandGraph& graph,
    u32 islandIndex,
    SolverWorkBuffers& workBuffers,
    const std::vector<DistanceConstraint>& distanceConstraints,
    f32 dt,
    f32 contactCompliance,
    const std::function<f32(const RigidBodySoA&, u32)>& invMassFn);

/// Batch guarded pipeline dispatch over nonsleeping islands; returns summary counts.
IslandBatchPipelineDispatchResult dispatch_all_islands_pipeline_result(
    RigidBodySoA& bodies,
    const ContactIslandGraph& graph,
    SolverWorkBuffers& workBuffers,
    const std::vector<DistanceConstraint>& distanceConstraints,
    f32 dt,
    f32 contactCompliance,
    const std::function<f32(const RigidBodySoA&, u32)>& invMassFn);

/// Human-readable labels for island reject reasons (logging / tests).
const char* island_dispatch_reject_reason_name(IslandDispatchRejectReason reason);
const char* island_job_dispatch_reject_reason_name(IslandJobDispatchRejectReason reason);
const char* island_constraint_solve_reject_reason_name(IslandConstraintSolveRejectReason reason);
const char* island_sleep_reject_reason_name(IslandSleepRejectReason reason);
const char* island_wake_reject_reason_name(IslandWakeRejectReason reason);

/// Diagnose why graph-level island dispatch would reject (B4.4 deepen follow-up pass).
IslandDispatchRejectReason island_dispatch_reject_reason(const ContactIslandGraph& graph, f32 dt);

/// Diagnose why per-job island dispatch would reject (B4.4 deepen follow-up pass).
IslandJobDispatchRejectReason island_job_dispatch_reject_reason(const IslandSolveJob& job, f32 dt);

/// Diagnose why per-island constraint solve would reject (B4.4 deepen follow-up pass).
IslandConstraintSolveRejectReason island_constraint_solve_reject_reason(
    const ContactIslandGraph::Island& island,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints);

/// Diagnose why per-island sleep preflight would skip solve (B4.4 deepen follow-up pass).
IslandSleepRejectReason island_sleep_reject_reason(const ContactIslandGraph::Island& island,
                                                   const RigidBodySoA& bodies);

/// Diagnose why per-island sleep preflight would skip by index (B4.4 deepen follow-up pass).
IslandSleepRejectReason island_sleep_reject_reason_by_index(const ContactIslandGraph& graph,
                                                            u32 islandIndex,
                                                            const RigidBodySoA& bodies);

/// Diagnose why per-island wake preflight would skip activation (B4.4 deepen follow-up pass).
IslandWakeRejectReason island_wake_reject_reason(const ContactIslandGraph::Island& island,
                                                 const RigidBodySoA& bodies);

/// Diagnose why per-island wake preflight would skip by index (B4.4 deepen follow-up pass).
IslandWakeRejectReason island_wake_reject_reason_by_index(const ContactIslandGraph& graph,
                                                          u32 islandIndex,
                                                          const RigidBodySoA& bodies);

/// Returns true when `island_dispatch_reject_reason` matches `expected` (B4.4 deepen follow-up pass).
bool island_dispatch_rejects_for_reason(const ContactIslandGraph& graph,
                                        f32 dt,
                                        IslandDispatchRejectReason expected);

/// Returns true when `island_job_dispatch_reject_reason` matches `expected` (B4.4 deepen follow-up pass).
bool island_job_dispatch_rejects_for_reason(const IslandSolveJob& job,
                                            f32 dt,
                                            IslandJobDispatchRejectReason expected);

/// Returns true when `island_constraint_solve_reject_reason` matches `expected` (B4.4 deepen follow-up pass).
bool island_constraint_solve_rejects_for_reason(
    const ContactIslandGraph::Island& island,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints,
    IslandConstraintSolveRejectReason expected);

/// Returns true when `island_sleep_reject_reason` matches `expected` (B4.4 deepen follow-up pass).
bool island_sleep_rejects_for_reason(const ContactIslandGraph::Island& island,
                                     const RigidBodySoA& bodies,
                                     IslandSleepRejectReason expected);

/// Returns true when `island_wake_reject_reason` matches `expected` (B4.4 deepen follow-up pass).
bool island_wake_rejects_for_reason(const ContactIslandGraph::Island& island,
                                    const RigidBodySoA& bodies,
                                    IslandWakeRejectReason expected);

/// Combined wake-then-solve pipeline preflight (B4.4 deepen follow-up pass).
struct IslandSolvePipelinePreflight {
    IslandWakeGraphPreflight wake{};
    IslandDispatchPreflight dispatch{};
    IslandSleepGraphPreflight sleep{};
    IslandDispatchRejectReason reason = IslandDispatchRejectReason::None;
    bool skipped = false;

    bool can_dispatch() const { return !skipped && reason == IslandDispatchRejectReason::None; }
};

/// Combined wake-then-solve pipeline outcome (B4.4 deepen follow-up pass).
struct IslandSolvePipelineResult {
    u32 wokeCount = 0;
    IslandBatchDispatchResult dispatch{};
    IslandDispatchRejectReason reason = IslandDispatchRejectReason::None;
    bool skipped = false;

    bool any_solved() const { return dispatch.any_solved(); }
};

/// Preflight wake-then-solve pipeline without mutating bodies (B4.4 deepen follow-up pass).
IslandSolvePipelinePreflight preflight_island_solve_pipeline(const ContactIslandGraph& graph,
                                                             const RigidBodySoA& bodies,
                                                             f32 dt);

/// Early-out guard for wake-then-solve pipeline dispatch (B4.4 deepen follow-up pass).
bool should_skip_island_solve_pipeline(const ContactIslandGraph& graph,
                                         const RigidBodySoA& bodies,
                                         f32 dt);

/// Guarded wake-then-solve pipeline over all dispatchable islands (B4.4 deepen follow-up pass).
IslandSolvePipelineResult dispatch_island_solve_pipeline(
    RigidBodySoA& bodies,
    const ContactIslandGraph& graph,
    SolverWorkBuffers& workBuffers,
    const std::vector<DistanceConstraint>& distanceConstraints,
    f32 dt,
    f32 contactCompliance,
    const std::function<f32(const RigidBodySoA&, u32)>& invMassFn);

/// Unified per-job dispatch preflight combining dt, constraint refs, body participation, and sleep.
IslandDispatchJobPreflight preflight_dispatch_island_job(
    const IslandSolveJob& job,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints,
    f32 dt);

/// Early-out guard for unified per-job dispatch preflight.
bool should_skip_dispatch_island_job(const IslandSolveJob& job,
                                     const RigidBodySoA& bodies,
                                     const std::vector<narrowphase::ContactManifold>& contacts,
                                     const std::vector<DistanceConstraint>& distanceConstraints,
                                     f32 dt);

/// Collect island indices that are dispatchable and not all-sleeping (parallel solve prep).
std::vector<u32> collect_solveable_island_indices(const ContactIslandGraph& graph,
                                                  const RigidBodySoA& bodies);

/// Guarded dispatch using unified preflight; skips stale refs, immovable bodies, and sleepers.
bool dispatch_solve_island_job_guarded(RigidBodySoA& bodies,
                                       const IslandSolveJob& job,
                                       SolverWorkBuffers& workBuffers,
                                       const std::vector<DistanceConstraint>& distanceConstraints,
                                       f32 dt,
                                       f32 contactCompliance,
                                       const std::function<f32(const RigidBodySoA&, u32)>& invMassFn);

/// Guarded dispatch with explicit skip/solve outcome and reject reason.
IslandDispatchResult dispatch_solve_island_job_guarded_result(
    RigidBodySoA& bodies,
    const IslandSolveJob& job,
    SolverWorkBuffers& workBuffers,
    const std::vector<DistanceConstraint>& distanceConstraints,
    f32 dt,
    f32 contactCompliance,
    const std::function<f32(const RigidBodySoA&, u32)>& invMassFn);

/// Combined wake + dispatch preflight for one island solve pipeline pass (B4.4 deepen follow-up pass).
struct IslandSolvePipelinePreflight {
    IslandWakeGraphPreflight wake{};
    IslandDispatchPreflight dispatch{};
    IslandSleepGraphPreflight sleep{};
    IslandDispatchRejectReason reason = IslandDispatchRejectReason::None;
    bool skipped = false;

    bool can_run() const { return !skipped && dispatch.can_dispatch(); }
};

/// Batch wake + dispatch summary for pipeline guarded helpers (B4.4 deepen follow-up pass).
struct IslandSolvePipelineResult {
    u32 wokeCount = 0;
    IslandBatchDispatchResult dispatch{};
    bool skipped = false;

    bool any_solved() const { return dispatch.any_solved(); }
};

/// Preflight wake-then-dispatch pipeline without mutating bodies or solver state.
IslandSolvePipelinePreflight preflight_island_solve_pipeline(const ContactIslandGraph& graph,
                                                               const RigidBodySoA& bodies,
                                                               f32 dt);

/// Early-out guard for the wake-then-dispatch island solve pipeline.
bool should_skip_island_solve_pipeline(const ContactIslandGraph& graph,
                                       const RigidBodySoA& bodies,
                                       f32 dt);

/// Guarded pipeline: wake mixed-sleep islands then dispatch constrained solves.
u32 dispatch_island_solve_pipeline_guarded(RigidBodySoA& bodies,
                                           const ContactIslandGraph& graph,
                                           SolverWorkBuffers& workBuffers,
                                           const std::vector<DistanceConstraint>& distanceConstraints,
                                           f32 dt,
                                           f32 contactCompliance,
                                           const std::function<f32(const RigidBodySoA&, u32)>& invMassFn);

/// Guarded pipeline with explicit wake and dispatch counts.
IslandSolvePipelineResult dispatch_island_solve_pipeline_result(
    RigidBodySoA& bodies,
    const ContactIslandGraph& graph,
    SolverWorkBuffers& workBuffers,
    const std::vector<DistanceConstraint>& distanceConstraints,
    f32 dt,
    f32 contactCompliance,
    const std::function<f32(const RigidBodySoA&, u32)>& invMassFn);

} // namespace fuse::physics
