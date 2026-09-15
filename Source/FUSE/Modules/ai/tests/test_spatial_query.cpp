#include <fuse/ai/spatial_query.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

void expectNear(float actual, float expected, float epsilon, const char* message) {
    if (std::fabs(actual - expected) > epsilon) {
        std::fprintf(stderr, "FAIL: %s (got %f, expected %f)\n", message, actual, expected);
        ++g_failures;
    }
}

std::vector<fuse::ai::AllyCandidate> makeSquad() {
    return {
        {0, 0.f, 0.f, 1},
        {1, 5.f, 0.f, 1},
        {2, 8.f, 0.f, 1},
        {3, 3.f, 4.f, 1},
        {4, 50.f, 0.f, 1},
        {5, 6.f, 0.f, 2},
    };
}

void testDistanceSq2d() {
    const float distSq = fuse::ai::distance_sq_2d(0.f, 0.f, 3.f, 4.f);
    expectNear(distSq, 25.f, 1e-4f, "distance_sq_2d returns squared length");
}

void testWithinRadius() {
    expectTrue(fuse::ai::within_radius(25.f, 5.f), "distance 5 is within radius 5");
    expectTrue(!fuse::ai::within_radius(26.f, 5.f), "distance > radius is outside");
    expectTrue(fuse::ai::within_radius(0.f, 0.f), "zero radius accepts co-located agents");
}

void testRadiusFilterIncludesNearbyAllies() {
    const std::vector<fuse::ai::AllyCandidate> allies = makeSquad();
    std::vector<fuse::u32> indices;

    const fuse::u32 count = fuse::ai::filter_allies_in_radius(0, 1, 0.f, 0.f, 10.f, allies, indices);
    expectTrue(count == 3u, "radius filter finds three allies within 10 units");
    expectTrue(indices.size() == 3u, "radius filter output size matches count");
    expectTrue(indices[0] == 1u || indices[0] == 2u || indices[0] == 3u,
               "radius filter returns ally indices");
}

void testRadiusFilterExcludesSelfAndOtherTeams() {
    const std::vector<fuse::ai::AllyCandidate> allies = makeSquad();
    std::vector<fuse::u32> indices;

    const fuse::u32 count = fuse::ai::filter_allies_in_radius(1, 1, 5.f, 0.f, 100.f, allies, indices);
    expectTrue(count == 4u, "radius filter excludes self but keeps same-team allies");
    for (fuse::u32 index : indices) {
        expectTrue(index != 1u, "radius filter never returns self index");
        expectTrue(index != 5u, "radius filter never returns other-team ally");
    }
}

void testRadiusFilterEmptyOutsideRange() {
    const std::vector<fuse::ai::AllyCandidate> allies = makeSquad();
    std::vector<fuse::u32> indices;

    const fuse::u32 count = fuse::ai::filter_allies_in_radius(0, 1, 0.f, 0.f, 2.f, allies, indices);
    expectTrue(count == 0u, "radius filter returns zero outside tight radius");
    expectTrue(indices.empty(), "radius filter clears output when none match");
}

void testRadiusFilterMinCountPolicy() {
    const std::vector<fuse::ai::AllyCandidate> allies = makeSquad();

    fuse::ai::RadiusFilterPolicy policy;
    policy.radius = 10.f;
    policy.minCount = 2;

    expectTrue(fuse::ai::allies_in_radius_satisfied(0, 1, 0.f, 0.f, policy, allies),
               "policy satisfied when enough allies in radius");
    policy.minCount = 4;
    expectTrue(!fuse::ai::allies_in_radius_satisfied(0, 1, 0.f, 0.f, policy, allies),
               "policy fails when minCount exceeds in-radius allies");
}

void testNearestAllyFindsClosest() {
    const std::vector<fuse::ai::AllyCandidate> allies = makeSquad();

    const fuse::ai::NearestAllyResult nearest =
        fuse::ai::find_nearest_ally(0, 1, 0.f, 0.f, allies);
    expectTrue(nearest.found, "nearest ally found for squad leader");
    expectTrue(nearest.allyIndex == 1u || nearest.allyIndex == 3u,
               "nearest ally is one of the equidistant close agents");
    expectNear(nearest.distanceSq, 25.f, 1e-4f, "nearest ally distance matches five-unit offset");
}

void testNearestAllyExcludesSelf() {
    const std::vector<fuse::ai::AllyCandidate> allies = makeSquad();

    const fuse::ai::NearestAllyResult nearest =
        fuse::ai::find_nearest_ally(1, 1, 5.f, 0.f, allies);
    expectTrue(nearest.found, "nearest ally found when querying from member");
    expectTrue(nearest.allyIndex != 1u, "nearest ally never returns self");
}

void testNearestAllyNoMatchOtherTeam() {
    const std::vector<fuse::ai::AllyCandidate> allies = makeSquad();

    const fuse::ai::NearestAllyResult nearest =
        fuse::ai::find_nearest_ally(5, 2, 6.f, 0.f, allies);
    expectTrue(!nearest.found, "nearest ally returns none when solo on team");
}

} // namespace

int run_spatial_query_tests() {
    testDistanceSq2d();
    testWithinRadius();
    testRadiusFilterIncludesNearbyAllies();
    testRadiusFilterExcludesSelfAndOtherTeams();
    testRadiusFilterEmptyOutsideRange();
    testRadiusFilterMinCountPolicy();
    testNearestAllyFindsClosest();
    testNearestAllyExcludesSelf();
    testNearestAllyNoMatchOtherTeam();
    return g_failures;
}
