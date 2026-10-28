#pragma once

// Collision edges of a tile solidity grid, wound like a one-sided physics edge chain.
//
// `solid` is row-major, index = y * width + x. Any nonzero byte is solid; 0 is empty. Bytes past
// the end of `solid` are empty. Cell (x, y) covers world
// [x * cellSize, (x + 1) * cellSize] x [y * cellSize, (y + 1) * cellSize], with +y up.
//
// An edge is emitted on each side where a solid cell meets an empty cell or the outside of the
// grid. The face between two solid cells is omitted. The segment runs from `a` to `b` so the solid
// cell is on the left: the outward normal (dy, -dx) points from solid toward empty, the same
// right-hand normal PhysicsWorld2D uses for one-sided chains (a counter-clockwise loop faces out).
// Order is increasing cell y, then x, then side: left, right, bottom, top.
// cellSize <= 0, or a grid with no cells, returns an empty vector.

#include <fuse/math/vec.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::world2d {

struct TileGrid {
    u32 width = 0;
    u32 height = 0;
    std::vector<u8> solid;
};

/// One directed boundary segment, from `a` to `b`.
struct TileEdge {
    fuse::math::Vec2 a{};
    fuse::math::Vec2 b{};
};

[[nodiscard]] inline std::vector<TileEdge> extract_tile_edges(const TileGrid& grid, f32 cellSize) {
    if (cellSize <= 0.f || grid.width == 0 || grid.height == 0 || grid.solid.empty()) {
        return {};
    }

    const auto solid = [&](s64 x, s64 y) {
        if (x < 0 || y < 0) {
            return false;
        }
        const u64 ux = static_cast<u64>(x);
        const u64 uy = static_cast<u64>(y);
        if (ux >= grid.width || uy >= grid.height) {
            return false;
        }
        const usize index = static_cast<usize>(uy) * static_cast<usize>(grid.width) + static_cast<usize>(ux);
        return index < grid.solid.size() && grid.solid[index] != 0;
    };

    std::vector<TileEdge> edges;
    for (u32 y = 0; y < grid.height; ++y) {
        for (u32 x = 0; x < grid.width; ++x) {
            const s64 ix = static_cast<s64>(x);
            const s64 iy = static_cast<s64>(y);
            if (!solid(ix, iy)) {
                continue;
            }

            const f32 x0 = static_cast<f32>(x) * cellSize;
            const f32 y0 = static_cast<f32>(y) * cellSize;
            const f32 x1 = x0 + cellSize;
            const f32 y1 = y0 + cellSize;

            // Left side, walk top -> bottom: solid (to the right) is on the left of the segment.
            if (!solid(ix - 1, iy)) {
                edges.push_back(TileEdge{fuse::math::Vec2{x0, y1}, fuse::math::Vec2{x0, y0}});
            }
            // Right side, walk bottom -> top.
            if (!solid(ix + 1, iy)) {
                edges.push_back(TileEdge{fuse::math::Vec2{x1, y0}, fuse::math::Vec2{x1, y1}});
            }
            // Bottom side, walk left -> right (outward normal points down).
            if (!solid(ix, iy - 1)) {
                edges.push_back(TileEdge{fuse::math::Vec2{x0, y0}, fuse::math::Vec2{x1, y0}});
            }
            // Top side, walk right -> left (outward normal points up).
            if (!solid(ix, iy + 1)) {
                edges.push_back(TileEdge{fuse::math::Vec2{x1, y1}, fuse::math::Vec2{x0, y1}});
            }
        }
    }
    return edges;
}

} // namespace fuse::world2d
