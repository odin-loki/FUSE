#pragma once

// Fog of war grid. Cells start hidden.
// reveal marks one in-range cell seen. Out-of-range coordinates do nothing,
// and seen() is false for them.
// reveal_radius reveals every cell whose Chebyshev distance
// (max of abs dx, abs dy) from the center is <= radius.
// A negative radius reveals nothing.

#include <fuse/types.hpp>

#include <algorithm>
#include <vector>

namespace fuse::gameplay {

class FogGrid {
public:
    FogGrid(i32 width, i32 height);

    void reveal(i32 x, i32 y);
    void reveal_radius(i32 cx, i32 cy, i32 radius);

    [[nodiscard]] bool seen(i32 x, i32 y) const;
    [[nodiscard]] std::size_t seen_count() const;

private:
    [[nodiscard]] bool in_range(i32 x, i32 y) const;
    [[nodiscard]] std::size_t index(i32 x, i32 y) const;

    i32 m_width = 0;
    i32 m_height = 0;
    std::size_t m_seenCount = 0;
    std::vector<u8> m_cells;
};

inline FogGrid::FogGrid(i32 width, i32 height) {
    if (width <= 0 || height <= 0) {
        return;
    }
    m_width = width;
    m_height = height;
    m_cells.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height), 0);
}

inline void FogGrid::reveal(i32 x, i32 y) {
    if (!in_range(x, y)) {
        return;
    }
    u8& cell = m_cells[index(x, y)];
    if (cell != 0) {
        return;
    }
    cell = 1;
    ++m_seenCount;
}

inline void FogGrid::reveal_radius(i32 cx, i32 cy, i32 radius) {
    if (radius < 0 || m_width <= 0 || m_height <= 0) {
        return;
    }
    const s64 rad = radius;
    const s64 x0 = std::max<s64>(0, static_cast<s64>(cx) - rad);
    const s64 y0 = std::max<s64>(0, static_cast<s64>(cy) - rad);
    const s64 x1 = std::min<s64>(static_cast<s64>(m_width) - 1, static_cast<s64>(cx) + rad);
    const s64 y1 = std::min<s64>(static_cast<s64>(m_height) - 1, static_cast<s64>(cy) + rad);
    if (x0 > x1 || y0 > y1) {
        return;
    }
    for (s64 y = y0; y <= y1; ++y) {
        for (s64 x = x0; x <= x1; ++x) {
            reveal(static_cast<i32>(x), static_cast<i32>(y));
        }
    }
}

inline bool FogGrid::seen(i32 x, i32 y) const {
    if (!in_range(x, y)) {
        return false;
    }
    return m_cells[index(x, y)] != 0;
}

inline std::size_t FogGrid::seen_count() const {
    return m_seenCount;
}

inline bool FogGrid::in_range(i32 x, i32 y) const {
    return x >= 0 && y >= 0 && x < m_width && y < m_height;
}

inline std::size_t FogGrid::index(i32 x, i32 y) const {
    return static_cast<std::size_t>(y) * static_cast<std::size_t>(m_width) + static_cast<std::size_t>(x);
}

} // namespace fuse::gameplay
