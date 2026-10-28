#include <fuse/world2d/tile_edges.hpp>
#include <fuse/world2d/tile_size.hpp>

#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

int g_failures = 0;

using fuse::math::Vec2;
using fuse::world2d::TileEdge;
using fuse::world2d::TileGrid;

struct ExpectedEdge {
    float ax;
    float ay;
    float bx;
    float by;
};

bool samePoint(Vec2 point, float x, float y) {
    return point.x == x && point.y == y;
}

/// Solid point is to the left of a -> b. Equivalent: outward normal (dy, -dx) points away from it.
bool solidOnLeft(const TileEdge& edge, Vec2 solidPoint) {
    const float dx = edge.b.x - edge.a.x;
    const float dy = edge.b.y - edge.a.y;
    const float vx = solidPoint.x - edge.a.x;
    const float vy = solidPoint.y - edge.a.y;
    return dx * vy - dy * vx > 0.f;
}

void expectEdges(const std::vector<TileEdge>& edges, const ExpectedEdge* want, std::size_t count, Vec2 interior,
                 const char* message) {
    if (edges.size() != count) {
        std::fprintf(stderr, "FAIL: %s (got %zu edges, expected %zu)\n", message, edges.size(), count);
        for (std::size_t i = 0; i < edges.size(); ++i) {
            std::fprintf(stderr, "  [%zu] (%.3f,%.3f)->(%.3f,%.3f)\n", i, static_cast<double>(edges[i].a.x),
                         static_cast<double>(edges[i].a.y), static_cast<double>(edges[i].b.x),
                         static_cast<double>(edges[i].b.y));
        }
        ++g_failures;
        return;
    }

    for (std::size_t i = 0; i < count; ++i) {
        const TileEdge& edge = edges[i];
        if (!samePoint(edge.a, want[i].ax, want[i].ay) || !samePoint(edge.b, want[i].bx, want[i].by)) {
            std::fprintf(stderr, "FAIL: %s (edge %zu got (%.3f,%.3f)->(%.3f,%.3f), expected (%.3f,%.3f)->(%.3f,%.3f))\n",
                         message, i, static_cast<double>(edge.a.x), static_cast<double>(edge.a.y),
                         static_cast<double>(edge.b.x), static_cast<double>(edge.b.y), static_cast<double>(want[i].ax),
                         static_cast<double>(want[i].ay), static_cast<double>(want[i].bx),
                         static_cast<double>(want[i].by));
            ++g_failures;
            return;
        }
        if (!solidOnLeft(edge, interior)) {
            std::fprintf(stderr, "FAIL: %s (edge %zu does not keep solid on the left)\n", message, i);
            ++g_failures;
            return;
        }
    }
}

void testEmptyInputs() {
    TileGrid grid;
    grid.width = 1;
    grid.height = 1;
    grid.solid = {1};

    if (!fuse::world2d::extract_tile_edges(grid, 0.f).empty()) {
        std::fprintf(stderr, "FAIL: cellSize 0 returns edges\n");
        ++g_failures;
    }
    if (!fuse::world2d::extract_tile_edges(grid, -2.f).empty()) {
        std::fprintf(stderr, "FAIL: negative cellSize returns edges\n");
        ++g_failures;
    }

    grid.width = 0;
    if (!fuse::world2d::extract_tile_edges(grid, 1.f).empty()) {
        std::fprintf(stderr, "FAIL: zero width returns edges\n");
        ++g_failures;
    }

    grid.width = 2;
    grid.height = 0;
    grid.solid = {1, 1};
    if (!fuse::world2d::extract_tile_edges(grid, 1.f).empty()) {
        std::fprintf(stderr, "FAIL: zero height returns edges\n");
        ++g_failures;
    }

    grid.height = 2;
    grid.solid.clear();
    if (!fuse::world2d::extract_tile_edges(grid, 1.f).empty()) {
        std::fprintf(stderr, "FAIL: empty solid vector returns edges\n");
        ++g_failures;
    }

    grid.solid = {0, 0, 0, 0};
    if (!fuse::world2d::extract_tile_edges(grid, 1.f).empty()) {
        std::fprintf(stderr, "FAIL: grid with no solid cells returns edges\n");
        ++g_failures;
    }
}

void testSingleSolidCell() {
    TileGrid grid;
    grid.width = 1;
    grid.height = 1;
    grid.solid = {1};

    const ExpectedEdge want[] = {
        {0.f, 2.f, 0.f, 0.f}, // left
        {2.f, 0.f, 2.f, 2.f}, // right
        {0.f, 0.f, 2.f, 0.f}, // bottom
        {2.f, 2.f, 0.f, 2.f}, // top
    };
    expectEdges(fuse::world2d::extract_tile_edges(grid, 2.f), want, 4, Vec2{1.f, 1.f},
                "single solid cell yields 4 edges");
}

void testSolidAgainstEmptyCell() {
    TileGrid grid;
    grid.width = 2;
    grid.height = 1;
    grid.solid = {1, 0};

    const ExpectedEdge want[] = {
        {0.f, 1.f, 0.f, 0.f}, // left, outside
        {1.f, 0.f, 1.f, 1.f}, // right, against the empty cell
        {0.f, 0.f, 1.f, 0.f}, // bottom
        {1.f, 1.f, 0.f, 1.f}, // top
    };
    expectEdges(fuse::world2d::extract_tile_edges(grid, 1.f), want, 4, Vec2{0.5f, 0.5f},
                "solid cell bordered by an empty cell");
}

void testTwoAdjacentSolidCells() {
    TileGrid grid;
    grid.width = 2;
    grid.height = 1;
    grid.solid = {1, 1};

    // The shared vertical face at x = 1 is omitted. Six per-cell outer segments remain.
    const ExpectedEdge want[] = {
        {0.f, 1.f, 0.f, 0.f}, // (0,0) left
        {0.f, 0.f, 1.f, 0.f}, // (0,0) bottom
        {1.f, 1.f, 0.f, 1.f}, // (0,0) top
        {2.f, 0.f, 2.f, 1.f}, // (1,0) right
        {1.f, 0.f, 2.f, 0.f}, // (1,0) bottom
        {2.f, 1.f, 1.f, 1.f}, // (1,0) top
    };
    expectEdges(fuse::world2d::extract_tile_edges(grid, 1.f), want, 6, Vec2{1.f, 0.5f},
                "two adjacent solid cells yield 6 edges");

    TileGrid stacked;
    stacked.width = 1;
    stacked.height = 2;
    stacked.solid = {1, 1};
    const ExpectedEdge stackedWant[] = {
        {0.f, 1.f, 0.f, 0.f}, // (0,0) left
        {1.f, 0.f, 1.f, 1.f}, // (0,0) right
        {0.f, 0.f, 1.f, 0.f}, // (0,0) bottom
        {0.f, 2.f, 0.f, 1.f}, // (0,1) left
        {1.f, 1.f, 1.f, 2.f}, // (0,1) right
        {1.f, 2.f, 0.f, 2.f}, // (0,1) top
    };
    expectEdges(fuse::world2d::extract_tile_edges(stacked, 1.f), stackedWant, 6, Vec2{0.5f, 1.f},
                "two vertically adjacent solid cells yield 6 edges");
}

void testFullySolid2x2OuterLoop() {
    TileGrid grid;
    grid.width = 2;
    grid.height = 2;
    grid.solid = {1, 1, 1, 1};

    // Four internal half-edges (the cross at x = 1 and y = 1) are omitted. Eight outer segments remain.
    const ExpectedEdge want[] = {
        {0.f, 1.f, 0.f, 0.f}, // (0,0) left
        {0.f, 0.f, 1.f, 0.f}, // (0,0) bottom
        {2.f, 0.f, 2.f, 1.f}, // (1,0) right
        {1.f, 0.f, 2.f, 0.f}, // (1,0) bottom
        {0.f, 2.f, 0.f, 1.f}, // (0,1) left
        {1.f, 2.f, 0.f, 2.f}, // (0,1) top
        {2.f, 1.f, 2.f, 2.f}, // (1,1) right
        {2.f, 2.f, 1.f, 2.f}, // (1,1) top
    };
    expectEdges(fuse::world2d::extract_tile_edges(grid, 1.f), want, 8, Vec2{1.f, 1.f},
                "fully solid 2x2 keeps only the outer loop");
}

void testTileSizeOk() {
    if (!fuse::world2d::tile_size_ok(8)) {
        std::fprintf(stderr, "FAIL: 8 px is a valid tile size\n");
        ++g_failures;
    }
    if (!fuse::world2d::tile_size_ok(32)) {
        std::fprintf(stderr, "FAIL: 32 px is a valid tile size\n");
        ++g_failures;
    }
    if (!fuse::world2d::tile_size_ok(256)) {
        std::fprintf(stderr, "FAIL: 256 px is a valid tile size\n");
        ++g_failures;
    }
    if (fuse::world2d::tile_size_ok(7)) {
        std::fprintf(stderr, "FAIL: 7 px is below the tile size range\n");
        ++g_failures;
    }
    if (fuse::world2d::tile_size_ok(24)) {
        std::fprintf(stderr, "FAIL: 24 px is not a power of two\n");
        ++g_failures;
    }
    if (fuse::world2d::tile_size_ok(512)) {
        std::fprintf(stderr, "FAIL: 512 px is above the tile size range\n");
        ++g_failures;
    }
}

} // namespace

int main() {
    testTileSizeOk();
    testEmptyInputs();
    testSingleSolidCell();
    testSolidAgainstEmptyCell();
    testTwoAdjacentSolidCells();
    testFullySolid2x2OuterLoop();

    if (g_failures == 0) {
        std::printf("fuse_world2d_tile_edges: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_world2d_tile_edges: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
