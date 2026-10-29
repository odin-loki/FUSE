#include <fuse/ai/nav_query.hpp>

#include <cmath>

namespace fuse::ai {

namespace {

/// Flood-fill from the agent's cell. False when the agent is off-grid or on a blocked cell.
bool build_field_(float x, float y, const navigation::GridNavmesh& mesh, std::vector<u32>& scratch) {
    navigation::Cell start;
    if (!mesh.cell_at(x, y, start)) {
        scratch.clear();
        return false;
    }
    return mesh.step_field(start, scratch);
}

/// Steps from the agent to `candidate`, or `kUnreachable` (off-grid, blocked, or walled off).
u32 steps_to_(const AllyCandidate& candidate, const navigation::GridNavmesh& mesh, const std::vector<u32>& field) {
    navigation::Cell cell;
    if (!mesh.cell_at(candidate.x, candidate.y, cell)) {
        return navigation::GridNavmesh::kUnreachable;
    }
    return mesh.steps_to(field, cell);
}

bool is_ally_(u32 selfIndex, u32 teamId, const AllyCandidate& candidate) {
    return candidate.agentIndex != selfIndex && candidate.teamId == teamId;
}

} // namespace

ReachableAllyResult find_nearest_reachable_ally(u32 selfIndex,
                                                u32 teamId,
                                                float x,
                                                float y,
                                                float maxPathLength,
                                                const std::vector<AllyCandidate>& allies,
                                                const navigation::GridNavmesh& mesh,
                                                std::vector<u32>& scratch) {
    ReachableAllyResult result;
    if (std::isnan(maxPathLength) || !build_field_(x, y, mesh, scratch)) {
        return result;
    }
    const bool limited = maxPathLength > 0.f;
    u32 bestSteps = navigation::GridNavmesh::kUnreachable;

    for (const AllyCandidate& candidate : allies) {
        if (!is_ally_(selfIndex, teamId, candidate)) {
            continue;
        }
        const u32 steps = steps_to_(candidate, mesh, scratch);
        if (steps == navigation::GridNavmesh::kUnreachable) {
            continue;
        }
        const float length = static_cast<float>(steps) * mesh.cell_size();
        if (limited && length > maxPathLength) {
            continue;
        }
        if (!result.found || steps < bestSteps || (steps == bestSteps && candidate.agentIndex < result.allyIndex)) {
            result.found = true;
            result.allyIndex = candidate.agentIndex;
            result.pathLength = length;
            bestSteps = steps;
        }
    }
    return result;
}

u32 count_reachable_allies(u32 selfIndex,
                           u32 teamId,
                           float x,
                           float y,
                           float maxPathLength,
                           const std::vector<AllyCandidate>& allies,
                           const navigation::GridNavmesh& mesh,
                           std::vector<u32>& scratch) {
    if (!std::isfinite(maxPathLength) || maxPathLength < 0.f || !build_field_(x, y, mesh, scratch)) {
        return 0;
    }
    u32 count = 0;
    for (const AllyCandidate& candidate : allies) {
        if (!is_ally_(selfIndex, teamId, candidate)) {
            continue;
        }
        const u32 steps = steps_to_(candidate, mesh, scratch);
        if (steps != navigation::GridNavmesh::kUnreachable &&
            static_cast<float>(steps) * mesh.cell_size() <= maxPathLength) {
            ++count;
        }
    }
    return count;
}

} // namespace fuse::ai
