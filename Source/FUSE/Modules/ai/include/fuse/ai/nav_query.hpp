#pragma once

#include <fuse/ai/spatial_query.hpp>
#include <fuse/navigation/grid_navmesh.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::ai {

/// Result of a navmesh-reachability ally lookup.
struct ReachableAllyResult {
    bool found = false;
    u32 allyIndex = 0;
    /// Grid path length in world units: 4-connected steps times the mesh cell size. Equal to the
    /// polyline length of `GridNavmesh::find_path` between the two cells.
    float pathLength = 0.f;
};

/// Nearest same-team ally (excluding `selfIndex`) by walking distance on `mesh`, not by straight
/// line. Positions map to cells with `GridNavmesh::cell_at` on XY; `z` is ignored.
/// Allies in blocked, walled-off, or off-grid cells are skipped; so is everyone when the agent's
/// own cell is off-grid or blocked. `maxPathLength > 0` drops allies farther than that walk;
/// zero or negative means unlimited. Ties break toward the lowest `agentIndex`.
/// One flood fill per call (`GridNavmesh::step_field` into `scratch`); `scratch` is reused across
/// calls to stay allocation-free after warm-up. Shares the mesh's search scratch, so calls on one
/// mesh must not run concurrently.
[[nodiscard]] ReachableAllyResult find_nearest_reachable_ally(u32 selfIndex,
                                                              u32 teamId,
                                                              float x,
                                                              float y,
                                                              float maxPathLength,
                                                              const std::vector<AllyCandidate>& allies,
                                                              const navigation::GridNavmesh& mesh,
                                                              std::vector<u32>& scratch);

/// Count same-team allies (excluding `selfIndex`) whose walk on `mesh` is at most `maxPathLength`.
/// `maxPathLength` must be finite and >= 0 (0 counts allies standing in the agent's own cell);
/// otherwise the count is 0. Same cell mapping, skip rules, and scratch contract as
/// `find_nearest_reachable_ally`.
[[nodiscard]] u32 count_reachable_allies(u32 selfIndex,
                                         u32 teamId,
                                         float x,
                                         float y,
                                         float maxPathLength,
                                         const std::vector<AllyCandidate>& allies,
                                         const navigation::GridNavmesh& mesh,
                                         std::vector<u32>& scratch);

} // namespace fuse::ai
