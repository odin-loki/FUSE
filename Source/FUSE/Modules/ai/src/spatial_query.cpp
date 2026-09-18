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

bool is_valid_ally_radius(float radius) {
    return radius > 0.f;
}

float effective_radius(const RadiusFilterPolicy& policy) {
    return clamp_radius_(policy.radius);
}

float radius_sq_from_policy(const RadiusFilterPolicy& policy) {
    return radius_sq_(effective_radius(policy));
}

u32 count_allies_in_radius(u32 selfIndex,
                           u32 teamId,
                           float x,
                           float y,
                           float radius,
                           const std::vector<AllyCandidate>& allies) {
    const float radiusSq = radius_sq_(radius);
    u32 count = 0;

    for (const AllyCandidate& candidate : allies) {
        if (!is_ally_(selfIndex, teamId, candidate)) {
            continue;
        }
        const float distSq = distance_sq_2d(x, y, candidate.x, candidate.y);
        if (distSq <= radiusSq) {
            ++count;
        }
    }

    return count;
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
        if (!result.found || distSq < bestSq ||
            (distSq == bestSq && candidate.agentIndex < result.allyIndex)) {
            result.found = true;
            result.allyIndex = candidate.agentIndex;
            result.distanceSq = distSq;
            bestSq = distSq;
        }
    }

    return result;
}

NearestAllyResult find_nearest_ally_within_radius(u32 selfIndex,
                                                  u32 teamId,
                                                  float x,
                                                  float y,
                                                  float maxRadius,
                                                  const std::vector<AllyCandidate>& allies) {
    const NearestAllyResult nearest = find_nearest_ally(selfIndex, teamId, x, y, allies);
    if (!nearest.found) {
        return nearest;
    }
    if (maxRadius > 0.f && !within_radius(nearest.distanceSq, maxRadius)) {
        return {};
    }
    return nearest;
}

bool allies_in_radius_satisfied(u32 selfIndex,
                                u32 teamId,
                                float x,
                                float y,
                                const RadiusFilterPolicy& policy,
                                const std::vector<AllyCandidate>& allies) {
    const u32 count = count_allies_in_radius(selfIndex,
                                             teamId,
                                             x,
                                             y,
                                             effective_radius(policy),
                                             allies);
    return count >= policy.minCount;
}

bool has_any_ally_in_radius(u32 selfIndex,
                            u32 teamId,
                            float x,
                            float y,
                            float radius,
                            const std::vector<AllyCandidate>& allies) {
    return count_allies_in_radius(selfIndex, teamId, x, y, radius, allies) > 0;
}

float nearest_ally_distance_sq(u32 selfIndex,
                             u32 teamId,
                             float x,
                             float y,
                             const std::vector<AllyCandidate>& allies) {
    const NearestAllyResult nearest = find_nearest_ally(selfIndex, teamId, x, y, allies);
    return nearest.found ? nearest.distanceSq : 0.f;
}

bool ally_context_available(const std::vector<AllyCandidate>* allies) {
    return allies != nullptr && !allies->empty();
}

bool is_radius_policy_valid(const RadiusFilterPolicy& policy) {
    return is_valid_ally_radius(policy.radius) && policy.minCount >= 1u;
}

bool ally_radius_query_valid(float radius, const std::vector<AllyCandidate>* allies) {
    return is_valid_ally_radius(radius) && ally_context_available(allies);
}

} // namespace fuse::ai
