#include <fuse/ai/spatial_query.hpp>

#include <algorithm>

namespace fuse::ai {

namespace {

float clamp_radius_(float radius) {
    return radius > 0.f ? radius : 0.f;
}

float radius_sq_(float radius) {
    const float clamped = clamp_radius_(radius);
    return clamped * clamped;
}

bool is_ally_(u32 selfIndex, u32 teamId, const AllyCandidate& candidate) {
    return candidate.agentIndex != selfIndex && candidate.teamId == teamId;
}

} // namespace

float distance_sq_2d(float ax, float ay, float bx, float by) {
    const float dx = ax - bx;
    const float dy = ay - by;
    return dx * dx + dy * dy;
}

bool within_radius(float distanceSq, float radius) {
    return distanceSq <= radius_sq_(radius);
}

float effective_radius(const RadiusFilterPolicy& policy) {
    return clamp_radius_(policy.radius);
}

u32 filter_allies_in_radius(u32 selfIndex,
                            u32 teamId,
                            float x,
                            float y,
                            float radius,
                            const std::vector<AllyCandidate>& allies,
                            std::vector<u32>& outIndices) {
    outIndices.clear();
    const float radiusSq = radius_sq_(radius);

    for (const AllyCandidate& candidate : allies) {
        if (!is_ally_(selfIndex, teamId, candidate)) {
            continue;
        }
        const float distSq = distance_sq_2d(x, y, candidate.x, candidate.y);
        if (distSq <= radiusSq) {
            outIndices.push_back(candidate.agentIndex);
        }
    }

    return static_cast<u32>(outIndices.size());
}

NearestAllyResult find_nearest_ally(u32 selfIndex,
                                    u32 teamId,
                                    float x,
                                    float y,
                                    const std::vector<AllyCandidate>& allies) {
    NearestAllyResult result;
    float bestSq = 0.f;

    for (const AllyCandidate& candidate : allies) {
        if (!is_ally_(selfIndex, teamId, candidate)) {
            continue;
        }

        const float distSq = distance_sq_2d(x, y, candidate.x, candidate.y);
        if (!result.found || distSq < bestSq) {
            result.found = true;
            result.allyIndex = candidate.agentIndex;
            result.distanceSq = distSq;
            bestSq = distSq;
        }
    }

    return result;
}

bool allies_in_radius_satisfied(u32 selfIndex,
                                u32 teamId,
                                float x,
                                float y,
                                const RadiusFilterPolicy& policy,
                                const std::vector<AllyCandidate>& allies) {
    std::vector<u32> scratch;
    const u32 count = filter_allies_in_radius(selfIndex,
                                              teamId,
                                              x,
                                              y,
                                              effective_radius(policy),
                                              allies,
                                              scratch);
    return count >= policy.minCount;
}

} // namespace fuse::ai
