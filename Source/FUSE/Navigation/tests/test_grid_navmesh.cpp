#include <fuse/navigation/grid_navmesh.hpp>
#include <fuse/navigation/nav_cell.hpp>
#include <fuse/navigation/path_cost.hpp>
#include <fuse/navigation/path_length.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <vector>

namespace {

int g_failures = 0;

void expect_true(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

bool near(fuse::math::Vec2 point, float x, float y) {
    return std::fabs(point.x - x) <= 1e-4f && std::fabs(point.y - y) <= 1e-4f;
}

void expect_point(fuse::math::Vec2 point, float x, float y, const char* message) {
    if (!near(point, x, y)) {
        std::fprintf(stderr, "FAIL: %s (got %.4f, %.4f expected %.4f, %.4f)\n", message, point.x, point.y, x,
                     y);
        ++g_failures;
    }
}

void expect_path(const std::vector<fuse::math::Vec2>& path, const std::vector<fuse::math::Vec2>& expected,
                 const char* message) {
    if (path.size() != expected.size()) {
        std::fprintf(stderr, "FAIL: %s (size %zu, expected %zu)\n", message, path.size(), expected.size());
        ++g_failures;
        return;
    }
    for (size_t i = 0; i < path.size(); ++i) {
        if (!near(path[i], expected[i].x, expected[i].y)) {
            std::fprintf(stderr, "FAIL: %s at %zu (got %.4f, %.4f expected %.4f, %.4f)\n", message, i, path[i].x,
                         path[i].y, expected[i].x, expected[i].y);
            ++g_failures;
            return;
        }
    }
}

void test_open_corridor() {
    fuse::navigation::GridNavmesh mesh;
    const bool built = mesh.build(5, 1, 2.f, [](fuse::u32, fuse::u32) { return false; });
    expect_true(built, "open corridor builds");

    const std::vector<fuse::math::Vec2> path = mesh.find_path({0, 0}, {4, 0});
    const std::vector<fuse::math::Vec2> expected = {
        {1.f, 1.f},
        {3.f, 1.f},
        {5.f, 1.f},
        {7.f, 1.f},
        {9.f, 1.f},
    };
    expect_path(path, expected, "open corridor centers");
    expect_path(mesh.find_path({0, 0}, {4, 0}), expected, "open corridor is deterministic");
}

void test_wall_with_one_cell_gap() {
    // Column x = 2 is a wall except the gap at (2, 2).
    // y2: . . o . .
    // y1: . . # . .
    // y0: S . # . G
    fuse::navigation::GridNavmesh mesh;
    const std::vector<fuse::navigation::Cell> blocked = {{2, 0}, {2, 1}};
    expect_true(mesh.build(5, 3, 1.f, blocked), "gapped wall builds");

    const std::vector<fuse::math::Vec2> path = mesh.find_path({0, 0}, {4, 0});
    expect_true(path.size() == 9, "gapped wall path is optimal length");
    if (!path.empty()) {
        expect_point(path.front(), 0.5f, 0.5f, "gapped wall starts at start center");
        expect_point(path.back(), 4.5f, 0.5f, "gapped wall ends at goal center");
    }

    bool saw_gap = false;
    bool entered_wall = false;
    for (const fuse::math::Vec2& point : path) {
        if (near(point, 2.5f, 2.5f)) {
            saw_gap = true;
        }
        if (near(point, 2.5f, 0.5f) || near(point, 2.5f, 1.5f)) {
            entered_wall = true;
        }
    }
    expect_true(saw_gap, "gapped wall path uses the gap");
    expect_true(!entered_wall, "gapped wall path does not enter blocked cells");

    for (size_t i = 1; i < path.size(); ++i) {
        const float dx = std::fabs(path[i].x - path[i - 1].x);
        const float dy = std::fabs(path[i].y - path[i - 1].y);
        const bool step = (dx <= 1e-4f && std::fabs(dy - 1.f) <= 1e-4f) ||
                          (dy <= 1e-4f && std::fabs(dx - 1.f) <= 1e-4f);
        expect_true(step, "gapped wall steps are 4-connected");
    }

    expect_true(mesh.find_path({0, 0}, {2, 0}).empty(), "blocked goal has no path");
    expect_true(mesh.find_path({2, 1}, {4, 0}).empty(), "blocked start has no path");
    expect_path(mesh.find_path({0, 0}, {4, 0}), path, "gapped wall path is deterministic");
}

void test_fully_blocked() {
    fuse::navigation::GridNavmesh mesh;
    const bool built = mesh.build(4, 4, 1.f, [](fuse::u32, fuse::u32) { return true; });
    expect_true(built, "fully blocked grid builds");
    expect_true(mesh.find_path({0, 0}, {3, 3}).empty(), "fully blocked grid has no path");
}

void test_start_equals_goal() {
    fuse::navigation::GridNavmesh mesh;
    expect_true(mesh.build(8, 8, 4.f, std::vector<fuse::navigation::Cell>{}), "start-goal grid builds");
    const std::vector<fuse::math::Vec2> path = mesh.find_path({3, 4}, {3, 4});
    expect_true(path.size() == 1, "start equals goal is one point");
    if (path.size() == 1) {
        expect_point(path[0], 14.f, 18.f, "start equals goal is that cell center");
    }

    fuse::navigation::GridNavmesh blocked;
    expect_true(blocked.build(2, 2, 1.f, [](fuse::u32, fuse::u32) { return true; }), "blocked stay grid builds");
    const std::vector<fuse::math::Vec2> stay = blocked.find_path({1, 0}, {1, 0});
    expect_true(stay.size() == 1, "start equals goal on a blocked cell is one point");
    if (stay.size() == 1) {
        expect_point(stay[0], 1.5f, 0.5f, "blocked start equals goal center");
    }
}

void test_tie_break_smaller_x_then_y() {
    // Open field. Equal-length routes exist; smaller x is expanded first, so the path
    // climbs the left edge and then crosses.
    fuse::navigation::GridNavmesh open;
    expect_true(open.build(3, 3, 1.f, [](fuse::u32, fuse::u32) { return false; }), "open field builds");
    const std::vector<fuse::math::Vec2> diagonal = open.find_path({0, 0}, {2, 2});
    const std::vector<fuse::math::Vec2> diagonal_expected = {
        {0.5f, 0.5f},
        {0.5f, 1.5f},
        {0.5f, 2.5f},
        {1.5f, 2.5f},
        {2.5f, 2.5f},
    };
    expect_path(diagonal, diagonal_expected, "equal f prefers smaller x");

    // Both detours around (1, 1) have length 4. Smaller y wins when x is tied.
    fuse::navigation::GridNavmesh detour;
    expect_true(detour.build(3, 3, 1.f, std::vector<fuse::navigation::Cell>{{1, 1}}), "detour grid builds");
    const std::vector<fuse::math::Vec2> around = detour.find_path({0, 1}, {2, 1});
    const std::vector<fuse::math::Vec2> around_expected = {
        {0.5f, 1.5f},
        {0.5f, 0.5f},
        {1.5f, 0.5f},
        {2.5f, 0.5f},
        {2.5f, 1.5f},
    };
    expect_path(around, around_expected, "equal f and x prefers smaller y");
}

void test_out_of_range_and_diagonal() {
    fuse::navigation::GridNavmesh mesh;
    expect_true(mesh.build(5, 1, 2.f, std::vector<fuse::navigation::Cell>{}), "range grid builds");
    expect_true(mesh.find_path({-1, 0}, {4, 0}).empty(), "negative start fails");
    expect_true(mesh.find_path({0, 0}, {0, 1}).empty(), "goal past height fails");
    expect_true(mesh.find_path({5, 0}, {5, 0}).empty(), "out-of-range start equals goal fails");
    expect_true(mesh.find_path({0, 0}, {4, 0}).size() == 5, "in-range corridor still paths");

    fuse::navigation::GridNavmesh fresh;
    expect_true(fresh.find_path({0, 0}, {0, 0}).empty(), "unbuilt mesh has no path");
    expect_true(!fresh.build(0, 4, 1.f, std::vector<fuse::navigation::Cell>{}), "zero width fails");
    expect_true(!mesh.build(4, 4, 0.f, std::vector<fuse::navigation::Cell>{}), "zero cell size fails");
    expect_true(mesh.find_path({0, 0}, {1, 0}).empty(), "failed rebuild clears the mesh");

    // Only a diagonal step joins the corners. 4-connected search cannot take it.
    fuse::navigation::GridNavmesh corners;
    expect_true(corners.build(2, 2, 1.f, std::vector<fuse::navigation::Cell>{{0, 1}, {1, 0}}),
                "diagonal block builds");
    expect_true(corners.find_path({0, 0}, {1, 1}).empty(), "diagonal neighbor is not a step");
}

int testPathCostOk() {
    if (!fuse::navigation::path_cost_ok(0)) {
        std::fprintf(stderr, "FAIL: 0 cost is ok\n");
        return 1;
    }
    if (!fuse::navigation::path_cost_ok(10000)) {
        std::fprintf(stderr, "FAIL: 10000 cost is ok\n");
        return 1;
    }
    if (fuse::navigation::path_cost_ok(-0.1f)) {
        std::fprintf(stderr, "FAIL: -0.1 cost is not ok\n");
        return 1;
    }
    if (fuse::navigation::path_cost_ok(10000.1f)) {
        std::fprintf(stderr, "FAIL: 10000.1 cost is not ok\n");
        return 1;
    }
    if (fuse::navigation::path_cost_ok(std::numeric_limits<float>::quiet_NaN())) {
        std::fprintf(stderr, "FAIL: NaN cost is not ok\n");
        return 1;
    }
    return 0;
}

void testNavCellOk() {
    expect_true(fuse::navigation::nav_cell_ok(0.1f), "0.1 m cell is ok");
    expect_true(fuse::navigation::nav_cell_ok(1), "1 m cell is ok");
    expect_true(fuse::navigation::nav_cell_ok(4), "4 m cell is ok");
    expect_true(!fuse::navigation::nav_cell_ok(0.05f), "0.05 m cell is not ok");
    expect_true(!fuse::navigation::nav_cell_ok(5), "5 m cell is not ok");
}

void testPathLength() {
    const std::vector<fuse::math::Vec2> empty;
    expect_true(fuse::navigation::path_length(empty) == 0.f, "empty path length is 0");

    const std::vector<fuse::math::Vec2> single = {{1.f, -2.f}};
    expect_true(fuse::navigation::path_length(single) == 0.f, "single point path length is 0");

    const std::vector<fuse::math::Vec2> three_four = {{0.f, 0.f}, {3.f, 4.f}};
    expect_true(std::fabs(fuse::navigation::path_length(three_four) - 5.f) <= 1e-4f, "(0,0) to (3,4) is 5");

    const std::vector<fuse::math::Vec2> longer = {{0.f, 0.f}, {3.f, 4.f}, {6.f, 8.f}};
    expect_true(std::fabs(fuse::navigation::path_length(longer) - 10.f) <= 1e-4f, "finite segments accumulate");

    const float inf = std::numeric_limits<float>::infinity();
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const std::vector<fuse::math::Vec2> broken = {
        {0.f, 0.f},
        {3.f, 4.f},
        {inf, 1.f},
        {6.f, 8.f},
        {nan, 0.f},
        {9.f, 0.f},
    };
    expect_true(std::fabs(fuse::navigation::path_length(broken) - 5.f) <= 1e-4f, "non-finite segment adds 0");
}

} // namespace

int main() {
    if (const int pathCostRc = testPathCostOk()) {
        return pathCostRc;
    }
    testNavCellOk();
    testPathLength();
    test_open_corridor();
    test_wall_with_one_cell_gap();
    test_fully_blocked();
    test_start_equals_goal();
    test_tie_break_smaller_x_then_y();
    test_out_of_range_and_diagonal();

    if (g_failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return EXIT_FAILURE;
    }
    std::printf("fuse_navigation_tests ok\n");
    return EXIT_SUCCESS;
}
