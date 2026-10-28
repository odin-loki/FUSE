#include <fuse/ai/query_radius.hpp>
#include <fuse/ai/same_team.hpp>
#include <fuse/ai/spatial_query.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
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

void testIsValidAllyRadius() {
    expectTrue(fuse::ai::is_valid_ally_radius(5.f), "positive radius is valid");
    expectTrue(!fuse::ai::is_valid_ally_radius(0.f), "zero radius is invalid");
    expectTrue(!fuse::ai::is_valid_ally_radius(-1.f), "negative radius is invalid");
}

void testRadiusSqFromPolicy() {
    fuse::ai::RadiusFilterPolicy policy;
    policy.radius = 5.f;
    expectNear(fuse::ai::radius_sq_from_policy(policy), 25.f, 1e-4f,
               "radius_sq_from_policy squares effective radius");
    policy.radius = -3.f;
    expectNear(fuse::ai::radius_sq_from_policy(policy), 0.f, 1e-4f,
               "radius_sq_from_policy clamps negative radius to zero");
}

void testNearestAllyDistanceSq() {
    const std::vector<fuse::ai::AllyCandidate> allies = makeSquad();

    const float nearestSq =
        fuse::ai::nearest_ally_distance_sq(0, 1, 0.f, 0.f, allies);
    expectNear(nearestSq, 25.f, 1e-4f, "nearest_ally_distance_sq returns closest ally distance");

    const float noneSq =
        fuse::ai::nearest_ally_distance_sq(5, 2, 6.f, 0.f, allies);
    expectNear(noneSq, 0.f, 1e-4f, "nearest_ally_distance_sq returns zero when no allies");
}

void testCountAlliesOutsideRadius() {
    const std::vector<fuse::ai::AllyCandidate> allies = makeSquad();

    const fuse::u32 outside =
        fuse::ai::count_allies_outside_radius(0, 1, 0.f, 0.f, 10.f, allies);
    expectTrue(outside == 1u, "count_allies_outside_radius finds distant ally");

    const fuse::u32 allOutside =
        fuse::ai::count_allies_outside_radius(0, 1, 0.f, 0.f, 2.f, allies);
    expectTrue(allOutside == 4u, "count_allies_outside_radius counts all beyond tight radius");
}

void testHasNoAlliesInRadius() {
    const std::vector<fuse::ai::AllyCandidate> allies = makeSquad();

    expectTrue(fuse::ai::has_no_allies_in_radius(0, 1, 0.f, 0.f, 2.f, allies),
               "has_no_allies_in_radius succeeds when none are nearby");
    expectTrue(!fuse::ai::has_no_allies_in_radius(0, 1, 0.f, 0.f, 100.f, allies),
               "has_no_allies_in_radius fails when allies are within radius");
    expectTrue(fuse::ai::has_no_allies_in_radius(0, 1, 0.f, 0.f, 10.f, {}),
               "has_no_allies_in_radius succeeds on empty ally list");
}

void testEffectiveMinCount() {
    fuse::ai::RadiusFilterPolicy policy;
    policy.minCount = 0;
    expectTrue(fuse::ai::effective_min_count(policy) == 1u,
               "effective_min_count defaults zero minCount to one");

    policy.minCount = 3;
    expectTrue(fuse::ai::effective_min_count(policy) == 3u,
               "effective_min_count preserves positive minCount");
}

void testIsFiniteAllyRadius() {
    expectTrue(fuse::ai::is_finite_ally_radius(3.f), "positive radius is finite");
    expectTrue(!fuse::ai::is_finite_ally_radius(0.f), "zero radius is not finite");
    expectTrue(!fuse::ai::is_finite_ally_radius(-2.f), "negative radius is not finite");
}

void testIsUnlimitedRadius() {
    expectTrue(fuse::ai::is_unlimited_radius(0.f), "zero radius is unlimited sentinel");
    expectTrue(fuse::ai::is_unlimited_radius(-1.f), "negative radius is unlimited sentinel");
    expectTrue(!fuse::ai::is_unlimited_radius(1.f), "positive radius is not unlimited");
}

void testRadiusFilterPolicyIsValid() {
    fuse::ai::RadiusFilterPolicy policy;
    policy.radius = 5.f;
    expectTrue(fuse::ai::radius_filter_policy_is_valid(policy),
               "radius filter policy succeeds with positive radius");
    policy.radius = 0.f;
    expectTrue(!fuse::ai::radius_filter_policy_is_valid(policy),
               "radius filter policy fails with zero radius");
}

void testNearestTieBreaksToLowestAgentIndex() {
    fuse::ai::FactionQuery factions;
    factions.selfTeam = 1;
    factions.hostileMask = 1u << 2;

    const std::vector<fuse::ai::AllyCandidate> hostiles = {
        {0, 0.f, 0.f, 1, 0.f, 1u << 1},
        {8, 0.f, 5.f, 2, 0.f, 1u << 2},
        {3, 5.f, 0.f, 2, 0.f, 1u << 2},
        {1, 40.f, 0.f, 2, 0.f, 1u << 2},
    };

    const fuse::ai::NearestAllyResult nearest =
        fuse::ai::find_nearest_hostile(0, factions, 0.f, 0.f, 0.f, hostiles);
    expectTrue(nearest.found, "nearest hostile found among equidistant enemies");
    expectTrue(nearest.allyIndex == 3u, "nearest hostile tie-break prefers lowest agent index");
    expectNear(nearest.distanceSq, 25.f, 1e-4f, "nearest hostile tie distance is five units");

    const std::vector<fuse::ai::AllyCandidate> allies = {
        {0, 0.f, 0.f, 1},
        {8, 0.f, 5.f, 1},
        {3, 5.f, 0.f, 1},
        {1, 40.f, 0.f, 1},
    };
    const fuse::ai::NearestAllyResult visible =
        fuse::ai::find_nearest_visible_ally(0, 1, 0.f, 0.f, 0.f, allies, {});
    expectTrue(visible.found, "visible ally found among equidistant allies");
    expectTrue(visible.allyIndex == 3u, "visible ally tie-break prefers lowest agent index");
    expectNear(visible.distanceSq, 25.f, 1e-4f, "visible ally tie distance is five units");
}

void testHostileMaskFilter() {
    fuse::ai::FactionQuery factions;
    factions.selfTeam = 1;
    factions.friendlyMask = (1u << 1) | (1u << 3);
    factions.hostileMask = (1u << 2) | (1u << 4);

    const std::vector<fuse::ai::AllyCandidate> agents = {
        {0, 0.f, 0.f, 2, 0.f, 1u << 2},
        {1, 1.f, 0.f, 1, 0.f, 1u << 2},
        {2, 1.f, 0.f, 3, 0.f, 1u << 3},
        {3, 1.f, 0.f, 4, 0.f, (1u << 2) | (1u << 3)},
        {4, 3.f, 0.f, 2, 0.f, 1u << 2},
        {5, 30.f, 0.f, 2, 0.f, 1u << 2},
        {6, 1.f, 0.f, 5, 0.f, 1u << 5},
        {7, 2.f, 0.f, 4, 0.f, 1u << 4},
        {9, 1.5f, 0.f, 2, 0.f, 0},
    };

    const fuse::u32 count =
        fuse::ai::count_hostiles_in_radius(0, factions, 0.f, 0.f, 0.f, 10.f, agents);
    expectTrue(count == 2u, "hostile mask counts only in-radius hostiles");

    const fuse::ai::NearestAllyResult nearest =
        fuse::ai::find_nearest_hostile(0, factions, 0.f, 0.f, 0.f, agents);
    expectTrue(nearest.found, "nearest hostile ignores friendlies, neutrals, and self");
    expectTrue(nearest.allyIndex == 7u, "nearest hostile is the closest mask match");
    expectNear(nearest.distanceSq, 4.f, 1e-4f, "nearest hostile distance excludes masked friendlies");

    factions.hostileMask = 0;
    expectTrue(fuse::ai::count_hostiles_in_radius(0, factions, 0.f, 0.f, 0.f, 10.f, agents) == 0u,
               "empty hostile mask counts no one");
    expectTrue(!fuse::ai::find_nearest_hostile(0, factions, 0.f, 0.f, 0.f, agents).found,
               "empty hostile mask finds no one");
}

void testHostileDistanceIncludesZ() {
    fuse::ai::FactionQuery factions;
    factions.selfTeam = 1;
    factions.hostileMask = 1u << 2;

    const std::vector<fuse::ai::AllyCandidate> agents = {
        {1, 2.f, 0.f, 2, 0.f, 1u << 2},
        {2, 0.f, 0.f, 2, 3.f, 1u << 2},
    };

    expectNear(fuse::ai::distance_sq_3d(0.f, 0.f, 0.f, 0.f, 0.f, 3.f), 9.f, 1e-4f,
               "distance_sq_3d includes height");

    const fuse::ai::NearestAllyResult nearest =
        fuse::ai::find_nearest_hostile(0, factions, 0.f, 0.f, 0.f, agents);
    expectTrue(nearest.found, "3D hostile query finds an enemy");
    expectTrue(nearest.allyIndex == 1u, "3D distance prefers the nearer hostile once z is included");
    expectNear(nearest.distanceSq, 4.f, 1e-4f, "hostile distance squares the planar offset");

    const fuse::u32 count =
        fuse::ai::count_hostiles_in_radius(0, factions, 0.f, 0.f, 0.f, 2.f, agents);
    expectTrue(count == 1u, "3D radius excludes a hostile that is near only on the XY plane");

    const fuse::ai::NearestAllyResult fromHeight =
        fuse::ai::find_nearest_hostile(0, factions, 0.f, 0.f, 3.f, agents);
    expectTrue(fromHeight.allyIndex == 2u, "hostile query measures distance from the seeker z");
    expectNear(fromHeight.distanceSq, 0.f, 1e-4f, "co-height hostile has zero 3D distance");
    expectTrue(fuse::ai::count_hostiles_in_radius(0, factions, 0.f, 0.f, 3.f, 1.f, agents) == 1u,
               "3D radius is measured from the seeker height");

    const std::vector<fuse::ai::AllyCandidate> squad = {
        {0, 0.f, 0.f, 1},
        {1, 5.f, 0.f, 1},
        {2, 1.f, 0.f, 1, 100.f},
    };
    const fuse::ai::NearestAllyResult xy = fuse::ai::find_nearest_ally(0, 1, 0.f, 0.f, squad);
    expectTrue(xy.allyIndex == 2u, "find_nearest_ally ignores z and stays on the XY plane");
    expectNear(xy.distanceSq, 1.f, 1e-4f, "XY nearest ally distance ignores height");
}

void testNearestVisibleAllySkipsBlockedNearer() {
    const std::vector<fuse::ai::AllyCandidate> allies = {
        {0, 0.f, 0.f, 1},
        {1, 4.f, 0.f, 1},
        {2, 4.f, 6.f, 1},
        {3, 1.f, 0.f, 2},
    };

    fuse::ai::AabbBlocker wall;
    wall.centerX = 2.f;
    wall.centerY = 0.f;
    wall.centerZ = 0.f;
    wall.halfX = 0.5f;
    wall.halfY = 0.5f;
    wall.halfZ = 0.5f;

    expectTrue(fuse::ai::segment_intersects_aabb(0.f, 0.f, 0.f, 4.f, 0.f, 0.f, wall),
               "slab test hits an AABB on the segment");
    expectTrue(!fuse::ai::segment_intersects_aabb(0.f, 0.f, 0.f, 4.f, 6.f, 0.f, wall),
               "slab test misses an AABB off the segment");
    expectTrue(!fuse::ai::segment_intersects_aabb(0.f, 0.f, 0.f, 1.f, 0.f, 0.f, wall),
               "slab test misses an AABB past the segment end");
    expectTrue(!fuse::ai::segment_intersects_aabb(0.f, 2.f, 0.f, 4.f, 2.f, 0.f, wall),
               "slab test misses when a parallel segment is outside the AABB");
    expectTrue(fuse::ai::segment_intersects_aabb(2.f, 0.f, 0.f, 2.2f, 0.f, 0.f, wall),
               "slab test hits when the segment starts inside the AABB");

    const std::vector<fuse::ai::AabbBlocker> blockers = {wall};
    const fuse::ai::NearestAllyResult visible =
        fuse::ai::find_nearest_visible_ally(0, 1, 0.f, 0.f, 0.f, allies, blockers);
    expectTrue(visible.found, "visible ally found when a farther ally is unobstructed");
    expectTrue(visible.allyIndex == 2u, "blocker hides the nearer ally so the farther visible one wins");
    expectNear(visible.distanceSq, 52.f, 1e-4f, "visible ally distance is the unobstructed length");

    const fuse::ai::NearestAllyResult clear =
        fuse::ai::find_nearest_visible_ally(0, 1, 0.f, 0.f, 0.f, allies, {});
    expectTrue(clear.found, "visible ally found with no blockers");
    expectTrue(clear.allyIndex == 1u, "without a blocker the nearer same-team ally wins");
    expectNear(clear.distanceSq, 16.f, 1e-4f, "clear line of sight keeps the nearer ally distance");

    const std::vector<fuse::ai::AllyCandidate> onlyNear = {
        {0, 0.f, 0.f, 1},
        {1, 4.f, 0.f, 1},
    };
    const fuse::ai::NearestAllyResult hidden =
        fuse::ai::find_nearest_visible_ally(0, 1, 0.f, 0.f, 0.f, onlyNear, blockers);
    expectTrue(!hidden.found, "a fully blocked ally is not visible");

    const fuse::ai::NearestAllyResult overWall =
        fuse::ai::find_nearest_visible_ally(0, 1, 0.f, 0.f, 2.f, onlyNear, blockers);
    expectTrue(overWall.found, "raised seeker can see over a ground blocker");
    expectTrue(overWall.allyIndex == 1u, "line of sight uses the seeker z");
}

void testSameTeam() {
    expectTrue(fuse::ai::same_team(0u, 0u), "team 0 matches team 0");
    expectTrue(fuse::ai::same_team(2u, 2u), "same_team(2, 2) is true");
    expectTrue(!fuse::ai::same_team(1u, 2u), "same_team(1, 2) is false");
}

void testQueryRadiusOk() {
    expectTrue(fuse::ai::query_radius_ok(0.5f), "query_radius_ok(0.5) is true");
    expectTrue(fuse::ai::query_radius_ok(128.f), "query_radius_ok(128) is true");
    expectTrue(!fuse::ai::query_radius_ok(0.f), "query_radius_ok(0) is false");
    expectTrue(!fuse::ai::query_radius_ok(-1.f), "query_radius_ok(-1) is false");
    expectTrue(!fuse::ai::query_radius_ok(128.1f), "query_radius_ok(128.1) is false");
    const float nan = std::numeric_limits<float>::quiet_NaN();
    expectTrue(!fuse::ai::query_radius_ok(nan), "query_radius_ok(NaN) is false");
}

} // namespace

int run_spatial_query_tests() {
    testQueryRadiusOk();
    if (g_failures != 0) {
        return g_failures;
    }
    testSameTeam();
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
    testIsValidAllyRadius();
    testRadiusSqFromPolicy();
    testNearestAllyDistanceSq();
    testCountAlliesOutsideRadius();
    testHasNoAlliesInRadius();
    testEffectiveMinCount();
    testIsFiniteAllyRadius();
    testIsUnlimitedRadius();
    testRadiusFilterPolicyIsValid();
    testNearestTieBreaksToLowestAgentIndex();
    testHostileMaskFilter();
    testHostileDistanceIncludesZ();
    testNearestVisibleAllySkipsBlockedNearer();
    return g_failures;
}
