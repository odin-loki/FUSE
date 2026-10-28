#pragma once

// Axis-aligned grid inventory. Cells are occupied or empty; items are rectangles.
// try_place scans top-left corners in row-major order (y, then x) and occupies the
// first rectangle that fits. remove clears a rectangle only when every cell in it
// is in range and occupied. A zero width or height grid rejects every placement.

#include <fuse/types.hpp>

#include <optional>
#include <vector>

namespace fuse::gameplay {

struct ItemSize {
    u32 w = 0;
    u32 h = 0;
};

struct GridSlot {
    u32 x = 0;
    u32 y = 0;
};

class InventoryGrid {
public:
    InventoryGrid(u32 width, u32 height);

    [[nodiscard]] std::optional<GridSlot> try_place(ItemSize size);
    [[nodiscard]] bool remove(GridSlot slot, ItemSize size);
    [[nodiscard]] u32 occupied_count() const;

private:
    [[nodiscard]] bool in_range(GridSlot slot, ItemSize size) const;
    [[nodiscard]] bool region_free(u32 x, u32 y, ItemSize size) const;
    [[nodiscard]] bool region_filled(u32 x, u32 y, ItemSize size) const;
    void set_region(u32 x, u32 y, ItemSize size, bool occupied);
    [[nodiscard]] std::size_t index(u32 x, u32 y) const;

    u32 m_width = 0;
    u32 m_height = 0;
    u32 m_filled = 0;
    std::vector<u8> m_cells;
};

inline InventoryGrid::InventoryGrid(u32 width, u32 height) {
    if (width == 0 || height == 0) {
        return;
    }
    m_width = width;
    m_height = height;
    m_cells.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height), 0);
}

inline std::optional<GridSlot> InventoryGrid::try_place(ItemSize size) {
    if (size.w == 0 || size.h == 0 || size.w > m_width || size.h > m_height) {
        return std::nullopt;
    }
    const u32 yLast = m_height - size.h;
    const u32 xLast = m_width - size.w;
    for (u32 y = 0; y <= yLast; ++y) {
        for (u32 x = 0; x <= xLast; ++x) {
            if (!region_free(x, y, size)) {
                continue;
            }
            set_region(x, y, size, true);
            return GridSlot{x, y};
        }
    }
    return std::nullopt;
}

inline bool InventoryGrid::remove(GridSlot slot, ItemSize size) {
    if (!in_range(slot, size) || !region_filled(slot.x, slot.y, size)) {
        return false;
    }
    set_region(slot.x, slot.y, size, false);
    return true;
}

inline u32 InventoryGrid::occupied_count() const {
    return m_filled;
}

inline bool InventoryGrid::in_range(GridSlot slot, ItemSize size) const {
    if (size.w == 0 || size.h == 0 || size.w > m_width || size.h > m_height) {
        return false;
    }
    return slot.x <= m_width - size.w && slot.y <= m_height - size.h;
}

inline bool InventoryGrid::region_free(u32 x, u32 y, ItemSize size) const {
    for (u32 dy = 0; dy < size.h; ++dy) {
        for (u32 dx = 0; dx < size.w; ++dx) {
            if (m_cells[index(x + dx, y + dy)] != 0) {
                return false;
            }
        }
    }
    return true;
}

inline bool InventoryGrid::region_filled(u32 x, u32 y, ItemSize size) const {
    for (u32 dy = 0; dy < size.h; ++dy) {
        for (u32 dx = 0; dx < size.w; ++dx) {
            if (m_cells[index(x + dx, y + dy)] == 0) {
                return false;
            }
        }
    }
    return true;
}

inline void InventoryGrid::set_region(u32 x, u32 y, ItemSize size, bool occupied) {
    const u8 value = occupied ? 1 : 0;
    for (u32 dy = 0; dy < size.h; ++dy) {
        for (u32 dx = 0; dx < size.w; ++dx) {
            m_cells[index(x + dx, y + dy)] = value;
        }
    }
    const u32 cells = size.w * size.h;
    if (occupied) {
        m_filled += cells;
    } else {
        m_filled -= cells;
    }
}

inline std::size_t InventoryGrid::index(u32 x, u32 y) const {
    return static_cast<std::size_t>(y) * static_cast<std::size_t>(m_width) + static_cast<std::size_t>(x);
}

} // namespace fuse::gameplay
