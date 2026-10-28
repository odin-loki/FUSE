#pragma once

#include <fuse/math/vec.hpp>
#include <fuse/types.hpp>

#include <concepts>
#include <utility>
#include <vector>

namespace fuse::navigation {

/// Integer cell coordinate. Cell (x, y) covers
/// [x * cell_size, (x + 1) * cell_size) by [y * cell_size, (y + 1) * cell_size).
struct Cell {
    s32 x = 0;
    s32 y = 0;

    constexpr bool operator==(const Cell& other) const { return x == other.x && y == other.y; }
};

/// Uniform 4-connected grid navmesh.
///
/// `find_path` is A* with step cost 1 and a Manhattan heuristic (consistent on this grid).
/// The open set pops the smallest f = g + h. Ties break toward smaller x, then smaller y.
/// A new route replaces the stored parent only when g is strictly smaller, so equal-cost
/// discoveries keep the parent expanded earlier under that order. The resulting waypoint
/// list is deterministic.
///
/// Waypoints are cell centers in world xy:
/// ((x + 0.5) * cell_size, (y + 0.5) * cell_size), from the start cell through the goal.
/// Blocked cells are not entered. An in-range start that equals the goal is that one center
/// and does not take a step, even if the cell is blocked. Endpoints outside the grid, a
/// blocked start that is not the goal, a blocked goal, or no connecting route yield an empty path.
///
/// `build` allocates the grid and the search scratch (g, parent, closed). After that, a query
/// allocates only the returned waypoint vector; the open set may allocate during the query and
/// is released before `find_path` returns. Queries share scratch and are not concurrent.
class GridNavmesh {
public:
    GridNavmesh() = default;

    /// `is_blocked(x, y)` is true when that cell cannot be entered.
    /// Invoked only for cells inside the grid. Fails (and clears any previous mesh) when
    /// width or height is 0, cell_size is not finite and positive, or the cell count does not
    /// fit in a 32-bit index with one sentinel value to spare.
    template <class Predicate>
        requires std::predicate<Predicate&, u32, u32>
    bool build(u32 width, u32 height, f32 cell_size, Predicate&& is_blocked);

    /// Cells listed in `blocked` cannot be entered. Coordinates outside the grid are ignored.
    bool build(u32 width, u32 height, f32 cell_size, const std::vector<Cell>& blocked);

    [[nodiscard]] std::vector<fuse::math::Vec2> find_path(Cell start, Cell goal) const;

private:
    static bool arguments_ok(u32 width, u32 height, f32 cell_size);
    bool adopt(u32 width, u32 height, f32 cell_size, std::vector<u8> blocked);
    void clear();

    [[nodiscard]] bool contains(Cell cell) const;
    [[nodiscard]] fuse::math::Vec2 center(u32 x, u32 y) const;

    u32 width_ = 0;
    u32 height_ = 0;
    f32 cell_size_ = 0.f;
    bool built_ = false;

    std::vector<u8> blocked_;
    mutable std::vector<u64> g_score_;
    mutable std::vector<u32> parent_;
    mutable std::vector<u8> closed_;
};

template <class Predicate>
    requires std::predicate<Predicate&, u32, u32>
bool GridNavmesh::build(u32 width, u32 height, f32 cell_size, Predicate&& is_blocked) {
    if (!arguments_ok(width, height, cell_size)) {
        clear();
        return false;
    }

    const u32 count = width * height;
    std::vector<u8> cells(static_cast<usize>(count), u8{0});
    for (u32 y = 0; y < height; ++y) {
        const u32 row = y * width;
        for (u32 x = 0; x < width; ++x) {
            if (is_blocked(x, y)) {
                cells[static_cast<usize>(row + x)] = u8{1};
            }
        }
    }
    return adopt(width, height, cell_size, std::move(cells));
}

} // namespace fuse::navigation
