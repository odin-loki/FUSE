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

} // namespace fuse::physics
