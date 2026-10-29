#include <fuse/ai/nav_query.hpp>

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

void expectNear(float actual, float expected, const char* message) {
    if (std::fabs(actual - expected) > 1e-4f) {
        std::fprintf(stderr, "FAIL: %s (got %f, expected %f)\n", message, actual, expected);
        ++g_failures;
    }
}

fuse::ai::AllyCandidate agent(fuse::u32 index, float x, float y, fuse::u32 team) {
    fuse::ai::AllyCandidate candidate;
    candidate.agentIndex = index;
    candidate.x = x;
    candidate.y = y;
    candidate.teamId = team;
    return candidate;
}

/// 6x4 grid, 1 m cells. A wall at x == 3 for y in [0, 2]; the only gap is (3, 3).
/// (5, 0) is sealed by (4, 0) and (5, 1).
fuse::navigation::GridNavmesh makeWalledMesh() {
    fuse::navigation::GridNavmesh mesh;
    const std::vector<fuse::navigation::Cell> blocked = {{3, 0}, {3, 1}, {3, 2}, {4, 0}, {5, 1}};
    const bool built = mesh.build(6, 4, 1.f, blocked);
    expectTrue(built, "walled mesh builds");
    return mesh;
}

void testWallMakesStraightLineNearestFarOnFoot() {
    const fuse::navigation::GridNavmesh mesh = makeWalledMesh();
    std::vector<fuse::u32> scratch;
    // Self in (2, 0). Ally 1 is 2 m away in a straight line, behind the wall at (4, 1):
    // walk (2,0)->(2,3)->(3,3)->(4,3)->(4,1) = 3 + 1 + 1 + 2 = 7 steps.
    // Ally 2 at (0, 2) is 2.8 m away in a straight line but only 4 steps on foot.
    const std::vector<fuse::ai::AllyCandidate> allies = {
        agent(0, 2.5f, 0.5f, 1),
        agent(1, 4.5f, 1.5f, 1),
        agent(2, 0.5f, 2.5f, 1),
        agent(3, 2.5f, 1.5f, 2), // other team, adjacent
    };

    const fuse::ai::NearestAllyResult straight = fuse::ai::find_nearest_ally(0, 1, 2.5f, 0.5f, allies);
    expectTrue(straight.found && straight.allyIndex == 1u, "straight-line nearest is behind the wall");

    const fuse::ai::ReachableAllyResult walk =
        fuse::ai::find_nearest_reachable_ally(0, 1, 2.5f, 0.5f, 0.f, allies, mesh, scratch);
    expectTrue(walk.found && walk.allyIndex == 2u, "walking nearest goes around the wall");
    expectNear(walk.pathLength, 4.f, "walk to ally 2 is 4 cells");

    const fuse::ai::ReachableAllyResult capped =
        fuse::ai::find_nearest_reachable_ally(0, 1, 2.5f, 0.5f, 3.5f, allies, mesh, scratch);
    expectTrue(!capped.found, "max path 3.5 m excludes the 4 m walk");

    const fuse::ai::ReachableAllyResult exact =
        fuse::ai::find_nearest_reachable_ally(0, 1, 2.5f, 0.5f, 4.f, allies, mesh, scratch);
    expectTrue(exact.found && exact.allyIndex == 2u, "max path is inclusive");

    expectTrue(fuse::ai::count_reachable_allies(0, 1, 2.5f, 0.5f, 6.f, allies, mesh, scratch) == 1u,
               "only ally 2 within a 6 m walk");
    expectTrue(fuse::ai::count_reachable_allies(0, 1, 2.5f, 0.5f, 7.f, allies, mesh, scratch) == 2u,
               "both allies within a 7 m walk");
    expectTrue(fuse::ai::count_reachable_allies(0, 2, 2.5f, 0.5f, 100.f, allies, mesh, scratch) == 1u,
               "team filter applies (team 2 sees only agent 3)");
}

void testUnreachableAndOffGridAlliesSkipped() {
    const fuse::navigation::GridNavmesh mesh = makeWalledMesh();
    std::vector<fuse::u32> scratch;
    const std::vector<fuse::ai::AllyCandidate> allies = {
        agent(0, 0.5f, 0.5f, 1),
        agent(1, 5.5f, 0.5f, 1),  // sealed cell
        agent(2, 3.5f, 1.5f, 1),  // standing in a wall cell
        agent(3, -1.f, 0.5f, 1),  // off-grid
        agent(4, std::numeric_limits<float>::quiet_NaN(), 0.5f, 1),
    };
    const fuse::ai::ReachableAllyResult walk =
        fuse::ai::find_nearest_reachable_ally(0, 1, 0.5f, 0.5f, 0.f, allies, mesh, scratch);
    expectTrue(!walk.found, "no reachable ally");
    expectTrue(fuse::ai::count_reachable_allies(0, 1, 0.5f, 0.5f, 1000.f, allies, mesh, scratch) == 0u,
               "unreachable allies are not counted");
}

void testSelfCellRules() {
    const fuse::navigation::GridNavmesh mesh = makeWalledMesh();
    std::vector<fuse::u32> scratch;
    const std::vector<fuse::ai::AllyCandidate> allies = {
        agent(0, 0.2f, 0.2f, 1),
        agent(7, 0.8f, 0.8f, 1), // same cell as self
        agent(5, 1.5f, 0.5f, 1),
    };
    const fuse::ai::ReachableAllyResult same =
        fuse::ai::find_nearest_reachable_ally(0, 1, 0.2f, 0.2f, 0.f, allies, mesh, scratch);
    expectTrue(same.found && same.allyIndex == 7u, "ally in the same cell wins");
    expectNear(same.pathLength, 0.f, "same cell is a 0 m walk");
    expectTrue(fuse::ai::count_reachable_allies(0, 1, 0.2f, 0.2f, 0.f, allies, mesh, scratch) == 1u,
               "zero max path counts same-cell allies");

    expectTrue(!fuse::ai::find_nearest_reachable_ally(0, 1, 3.5f, 0.5f, 0.f, allies, mesh, scratch).found,
               "self on a blocked cell finds nobody");
    expectTrue(!fuse::ai::find_nearest_reachable_ally(0, 1, 10.f, 0.5f, 0.f, allies, mesh, scratch).found,
               "self off-grid finds nobody");
    expectTrue(scratch.empty(), "failed lookups clear the scratch field");

    fuse::navigation::GridNavmesh unbuilt;
    expectTrue(!fuse::ai::find_nearest_reachable_ally(0, 1, 0.2f, 0.2f, 0.f, allies, unbuilt, scratch).found,
               "unbuilt mesh finds nobody");

    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    expectTrue(!fuse::ai::find_nearest_reachable_ally(0, 1, 0.2f, 0.2f, nan, allies, mesh, scratch).found,
               "NaN max path finds nobody");
    expectTrue(fuse::ai::find_nearest_reachable_ally(0, 1, 0.2f, 0.2f, inf, allies, mesh, scratch).found,
               "infinite max path is unlimited");
    expectTrue(fuse::ai::count_reachable_allies(0, 1, 0.2f, 0.2f, -1.f, allies, mesh, scratch) == 0u,
               "negative count radius counts nothing");
    expectTrue(fuse::ai::count_reachable_allies(0, 1, 0.2f, 0.2f, inf, allies, mesh, scratch) == 0u,
               "infinite count radius is rejected");
}

void testTieBreaksToLowestIndex() {
    fuse::navigation::GridNavmesh mesh;
    expectTrue(mesh.build(5, 5, 2.f, [](fuse::u32, fuse::u32) { return false; }), "open mesh builds");
    std::vector<fuse::u32> scratch;
    // Self at cell (2, 2); allies at (4, 2) and (2, 0) are both 2 steps (4 m with 2 m cells).
    const std::vector<fuse::ai::AllyCandidate> allies = {
        agent(9, 9.f, 5.f, 3),
        agent(4, 5.f, 1.f, 3),
        agent(1, 5.f, 5.f, 3),
    };
    const fuse::ai::ReachableAllyResult walk =
        fuse::ai::find_nearest_reachable_ally(1, 3, 5.f, 5.f, 0.f, allies, mesh, scratch);
    expectTrue(walk.found && walk.allyIndex == 4u, "equal walks break toward the lower agentIndex");
    expectNear(walk.pathLength, 4.f, "path length scales with cell size");
}

} // namespace

int main() {
    testWallMakesStraightLineNearestFarOnFoot();
    testUnreachableAndOffGridAlliesSkipped();
    testSelfCellRules();
    testTieBreaksToLowestIndex();
    if (g_failures != 0) {
        std::fprintf(stderr, "fuse_ai_nav_query_tests: %d failure(s)\n", g_failures);
        return EXIT_FAILURE;
    }
    std::printf("fuse_ai_nav_query_tests: all checks passed\n");
    return EXIT_SUCCESS;
}
