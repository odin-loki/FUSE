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

void testCountAlliesInRadiusMatchesFilter() {
    const std::vector<fuse::ai::AllyCandidate> allies = makeSquad();
    std::vector<fuse::u32> indices;

    const fuse::u32 count =
        fuse::ai::count_allies_in_radius(0, 1, 0.f, 0.f, 10.f, allies);
    const fuse::u32 filtered =
        fuse::ai::filter_allies_in_radius(0, 1, 0.f, 0.f, 10.f, allies, indices);
    expectTrue(count == filtered, "count_allies_in_radius matches filter_allies_in_radius");
    expectTrue(count == 3u, "count finds three allies within radius");
}

void testRadiusFilterEmptyAllyList() {
    const std::vector<fuse::ai::AllyCandidate> allies;
    std::vector<fuse::u32> indices;

    const fuse::u32 count =
        fuse::ai::count_allies_in_radius(0, 1, 0.f, 0.f, 10.f, allies);
    expectTrue(count == 0u, "count returns zero for empty ally list");
    expectTrue(fuse::ai::filter_allies_in_radius(0, 1, 0.f, 0.f, 10.f, allies, indices) == 0u,
               "filter returns zero for empty ally list");
    expectTrue(indices.empty(), "filter output stays empty");

    fuse::ai::RadiusFilterPolicy policy;
    policy.radius = 10.f;
    policy.minCount = 1;
    expectTrue(!fuse::ai::allies_in_radius_satisfied(0, 1, 0.f, 0.f, policy, allies),
               "radius policy fails on empty ally list");
}

void testNearestAllyFindsClosest() {
    const std::vector<fuse::ai::AllyCandidate> allies = makeSquad();

    const fuse::ai::NearestAllyResult nearest =
        fuse::ai::find_nearest_ally(0, 1, 0.f, 0.f, allies);
    expectTrue(nearest.found, "nearest ally found for squad leader");
    expectTrue(nearest.allyIndex == 1u, "nearest ally tie-break prefers lowest agent index");
    expectNear(nearest.distanceSq, 25.f, 1e-4f, "nearest ally distance matches five-unit offset");
}

void testNearestAllyWithinRadiusRejectsDistant() {
    const std::vector<fuse::ai::AllyCandidate> allies = makeSquad();

    const fuse::ai::NearestAllyResult within =
        fuse::ai::find_nearest_ally_within_radius(0, 1, 0.f, 0.f, 6.f, allies);
    expectTrue(within.found, "nearest ally within radius succeeds");
    expectTrue(within.allyIndex == 1u, "within-radius nearest is closest ally");

    const fuse::ai::NearestAllyResult beyond =
        fuse::ai::find_nearest_ally_within_radius(0, 1, 0.f, 0.f, 4.f, allies);
    expectTrue(!beyond.found, "nearest ally within radius rejects beyond max");
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

void testHasAnyAllyInRadius() {
    const std::vector<fuse::ai::AllyCandidate> allies = makeSquad();

    expectTrue(fuse::ai::has_any_ally_in_radius(0, 1, 0.f, 0.f, 10.f, allies),
               "has_any_ally_in_radius succeeds when allies are nearby");
    expectTrue(!fuse::ai::has_any_ally_in_radius(0, 1, 0.f, 0.f, 2.f, allies),
               "has_any_ally_in_radius fails outside tight radius");
    expectTrue(!fuse::ai::has_any_ally_in_radius(0, 1, 0.f, 0.f, 10.f, {}),
               "has_any_ally_in_radius fails on empty ally list");
}

void testAllyContextAvailable() {
    const std::vector<fuse::ai::AllyCandidate> allies = makeSquad();
    expectTrue(!fuse::ai::ally_context_available(nullptr), "ally context unavailable when null");
    expectTrue(fuse::ai::ally_context_available(&allies),
               "ally context available when list is non-empty");
    const std::vector<fuse::ai::AllyCandidate> empty;
    expectTrue(!fuse::ai::ally_context_available(&empty), "ally context unavailable when empty");
}

} // namespace

int run_spatial_query_tests() {
    testDistanceSq2d();
    testWithinRadius();
    testCountAlliesInRadiusMatchesFilter();
    testRadiusFilterIncludesNearbyAllies();
    testRadiusFilterExcludesSelfAndOtherTeams();
    testRadiusFilterEmptyOutsideRange();
    testRadiusFilterEmptyAllyList();
    testRadiusFilterMinCountPolicy();
    testNearestAllyFindsClosest();
    testNearestAllyWithinRadiusRejectsDistant();
    testNearestAllyExcludesSelf();
    testNearestAllyNoMatchOtherTeam();
    testHasAnyAllyInRadius();
    testAllyContextAvailable();
    return g_failures;
}
