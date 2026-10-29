#include <fuse/navigation/grid_navmesh.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <queue>
#include <utility>
#include <vector>

namespace fuse::navigation {
namespace {

constexpr u32 kNone = 0xffffffffu;
constexpr u64 kUnvisited = std::numeric_limits<u64>::max();

/// Neighbor order is smaller x, then smaller y. The heap comparator is what makes
/// equal-f pops deterministic; this order only documents the same preference.
constexpr s32 kNeighbor[4][2] = {
    {-1, 0},
    {0, -1},
    {0, 1},
    {1, 0},
};

struct OpenNode {
    u64 f = 0;
    u64 g = 0;
    u32 index = 0;
    s32 x = 0;
    s32 y = 0;
};

/// Min-heap: smallest f, then smallest x, then smallest y.
struct OpenOrder {
    bool operator()(const OpenNode& a, const OpenNode& b) const {
        if (a.f != b.f) {
            return a.f > b.f;
        }
        if (a.x != b.x) {
            return a.x > b.x;
        }
        return a.y > b.y;
    }
};

u64 manhattan(s32 ax, s32 ay, s32 bx, s32 by) {
    const s64 dx = static_cast<s64>(ax) - static_cast<s64>(bx);
    const s64 dy = static_cast<s64>(ay) - static_cast<s64>(by);
    const u64 adx = static_cast<u64>(dx < 0 ? -dx : dx);
    const u64 ady = static_cast<u64>(dy < 0 ? -dy : dy);
    return adx + ady;
}

} // namespace

bool GridNavmesh::arguments_ok(u32 width, u32 height, f32 cell_size) {
    if (width == 0 || height == 0) {
        return false;
    }
    if (!(cell_size > 0.f) || !std::isfinite(cell_size)) {
        return false;
    }
    const u64 count = static_cast<u64>(width) * static_cast<u64>(height);
    return count <= static_cast<u64>(std::numeric_limits<u32>::max()) - 1u;
}

bool GridNavmesh::adopt(u32 width, u32 height, f32 cell_size, std::vector<u8> blocked) {
    const u64 count64 = static_cast<u64>(width) * static_cast<u64>(height);
    if (!arguments_ok(width, height, cell_size) || static_cast<u64>(blocked.size()) != count64) {
        clear();
        return false;
    }

    const usize count = blocked.size();
    std::vector<u64> g_score(count, kUnvisited);
    std::vector<u32> parent(count, kNone);
    std::vector<u8> closed(count, u8{0});

    blocked_ = std::move(blocked);
    g_score_ = std::move(g_score);
    parent_ = std::move(parent);
    closed_ = std::move(closed);
    width_ = width;
    height_ = height;
    cell_size_ = cell_size;
    built_ = true;
    return true;
}

void GridNavmesh::clear() {
    width_ = 0;
    height_ = 0;
    cell_size_ = 0.f;
    built_ = false;
    std::vector<u8>().swap(blocked_);
    std::vector<u64>().swap(g_score_);
    std::vector<u32>().swap(parent_);
    std::vector<u8>().swap(closed_);
}

bool GridNavmesh::contains(Cell cell) const {
    return cell.x >= 0 && cell.y >= 0 && static_cast<u32>(cell.x) < width_ &&
           static_cast<u32>(cell.y) < height_;
}

fuse::math::Vec2 GridNavmesh::center(u32 x, u32 y) const {
    return {(static_cast<f32>(x) + 0.5f) * cell_size_, (static_cast<f32>(y) + 0.5f) * cell_size_};
}

bool GridNavmesh::build(u32 width, u32 height, f32 cell_size, const std::vector<Cell>& blocked) {
    if (!arguments_ok(width, height, cell_size)) {
        clear();
        return false;
    }

    const u32 count = width * height;
    std::vector<u8> cells(static_cast<usize>(count), u8{0});
    for (const Cell& cell : blocked) {
        if (cell.x < 0 || cell.y < 0) {
            continue;
        }
        const u32 x = static_cast<u32>(cell.x);
        const u32 y = static_cast<u32>(cell.y);
        if (x >= width || y >= height) {
            continue;
        }
        cells[static_cast<usize>(y) * width + x] = u8{1};
    }
    return adopt(width, height, cell_size, std::move(cells));
}

std::vector<fuse::math::Vec2> GridNavmesh::find_path(Cell start, Cell goal) const {
    if (!built_ || !contains(start) || !contains(goal)) {
        return {};
    }
    if (start == goal) {
        return {center(static_cast<u32>(start.x), static_cast<u32>(start.y))};
    }

    const u32 start_index = static_cast<u32>(start.y) * width_ + static_cast<u32>(start.x);
    const u32 goal_index = static_cast<u32>(goal.y) * width_ + static_cast<u32>(goal.x);
    if (blocked_[start_index] != 0 || blocked_[goal_index] != 0) {
        return {};
    }

    const usize count = g_score_.size();
    std::fill(g_score_.begin(), g_score_.end(), kUnvisited);
    std::fill(parent_.begin(), parent_.end(), kNone);
    std::fill(closed_.begin(), closed_.end(), u8{0});

    g_score_[start_index] = 0;
    parent_[start_index] = kNone;

    std::priority_queue<OpenNode, std::vector<OpenNode>, OpenOrder> open;
    open.push(OpenNode{
        manhattan(start.x, start.y, goal.x, goal.y),
        0,
        start_index,
        start.x,
        start.y,
    });

    bool found = false;
    while (!open.empty()) {
        const OpenNode node = open.top();
        open.pop();
        if (closed_[node.index] != 0 || node.g != g_score_[node.index]) {
            continue;
        }
        closed_[node.index] = 1;
        if (node.index == goal_index) {
            found = true;
            break;
        }

        for (const auto& delta : kNeighbor) {
            const s32 nx = node.x + delta[0];
            const s32 ny = node.y + delta[1];
            if (nx < 0 || ny < 0 || static_cast<u32>(nx) >= width_ || static_cast<u32>(ny) >= height_) {
                continue;
            }
            const u32 nindex =
                static_cast<u32>(static_cast<u64>(ny) * static_cast<u64>(width_) + static_cast<u32>(nx));
            if (blocked_[nindex] != 0 || closed_[nindex] != 0) {
                continue;
            }
            const u64 ng = node.g + 1;
            if (ng >= g_score_[nindex]) {
                continue;
            }
            g_score_[nindex] = ng;
            parent_[nindex] = node.index;
            open.push(OpenNode{
                ng + manhattan(nx, ny, goal.x, goal.y),
                ng,
                nindex,
                nx,
                ny,
            });
        }
    }

    if (!found) {
        return {};
    }

    u32 steps = 0;
    for (u32 at = goal_index;;) {
        ++steps;
        if (steps > count) {
            return {};
        }
        const u32 previous = parent_[at];
        if (previous == kNone) {
            break;
        }
        at = previous;
    }

    std::vector<fuse::math::Vec2> path(static_cast<usize>(steps));
    u32 at = goal_index;
    for (u32 i = steps; i > 0; --i) {
        const u32 x = at % width_;
        const u32 y = at / width_;
        path[static_cast<usize>(i - 1)] = center(x, y);
        if (i == 1) {
            break;
        }
        at = parent_[at];
    }
    return path;
}

bool GridNavmesh::step_field(Cell start, std::vector<u32>& out) const {
    if (!built_ || !contains(start)) {
        out.clear();
        return false;
    }
    const u32 start_index = static_cast<u32>(start.y) * width_ + static_cast<u32>(start.x);
    if (blocked_[start_index] != 0) {
        out.clear();
        return false;
    }

    out.assign(blocked_.size(), kUnreachable);
    out[start_index] = 0;

    // Each cell is enqueued at most once, so the parent scratch (one slot per cell) is the queue.
    std::vector<u32>& queue = parent_;
    usize head = 0;
    usize tail = 0;
    queue[tail++] = start_index;
    while (head < tail) {
        const u32 index = queue[head++];
        const s32 x = static_cast<s32>(index % width_);
        const s32 y = static_cast<s32>(index / width_);
        const u32 next_steps = out[index] + 1u;
        for (const auto& delta : kNeighbor) {
            const s32 nx = x + delta[0];
            const s32 ny = y + delta[1];
            if (nx < 0 || ny < 0 || static_cast<u32>(nx) >= width_ || static_cast<u32>(ny) >= height_) {
                continue;
            }
            const u32 nindex = static_cast<u32>(ny) * width_ + static_cast<u32>(nx);
            if (blocked_[nindex] != 0 || out[nindex] != kUnreachable) {
                continue;
            }
            out[nindex] = next_steps;
            queue[tail++] = nindex;
        }
    }
    return true;
}

u32 GridNavmesh::steps_to(const std::vector<u32>& field, Cell cell) const {
    if (!built_ || !contains(cell) || field.size() != blocked_.size()) {
        return kUnreachable;
    }
    return field[static_cast<usize>(cell.y) * width_ + static_cast<u32>(cell.x)];
}

bool GridNavmesh::cell_at(f32 x, f32 y, Cell& out) const {
    if (!built_ || !std::isfinite(x) || !std::isfinite(y)) {
        return false;
    }
    const f64 fx = std::floor(static_cast<f64>(x) / static_cast<f64>(cell_size_));
    const f64 fy = std::floor(static_cast<f64>(y) / static_cast<f64>(cell_size_));
    if (fx < 0.0 || fy < 0.0 || fx >= static_cast<f64>(width_) || fy >= static_cast<f64>(height_)) {
        return false;
    }
    out = Cell{static_cast<s32>(fx), static_cast<s32>(fy)};
    return true;
}

} // namespace fuse::navigation
