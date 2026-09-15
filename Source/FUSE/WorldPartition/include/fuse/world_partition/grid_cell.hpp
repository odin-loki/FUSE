#pragma once

#include <fuse/ecs/entity.hpp>
#include <fuse/spatial/aabb.hpp>
#include <fuse/types.hpp>

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace fuse::world_partition {

/// Integer grid coordinate on the XZ world plane (Y is up).
struct GridCoord {
    s32 x = 0;
    s32 y = 0;

    bool operator==(const GridCoord& other) const { return x == other.x && y == other.y; }
    bool operator!=(const GridCoord& other) const { return !(*this == other); }
};

[[nodiscard]] inline u64 grid_coord_key(GridCoord coord) {
    return static_cast<u64>(static_cast<u32>(coord.x)) |
           (static_cast<u64>(static_cast<u32>(coord.y)) << 32u);
}

[[nodiscard]] inline GridCoord world_to_grid(fuse::ecs::vec3 world_pos, f32 cell_size) {
    if (cell_size <= 0.f) {
        return {};
    }
    const f32 inv = 1.f / cell_size;
    return {static_cast<s32>(std::floor(world_pos.x * inv)),
            static_cast<s32>(std::floor(world_pos.z * inv))};
}

[[nodiscard]] inline fuse::ecs::vec3 grid_to_world_center(GridCoord coord, f32 cell_size) {
    const f32 half = cell_size * 0.5f;
    return {static_cast<f32>(coord.x) * cell_size + half, 0.f,
            static_cast<f32>(coord.y) * cell_size + half, 0.f};
}

[[nodiscard]] inline fuse::spatial::AABB grid_cell_bounds(GridCoord coord, f32 cell_size) {
    const f32 min_x = static_cast<f32>(coord.x) * cell_size;
    const f32 min_z = static_cast<f32>(coord.y) * cell_size;
    return {{min_x, 0.f, min_z, 0.f},
            {min_x + cell_size, cell_size, min_z + cell_size, 0.f}};
}

/// Residency lifecycle for a world partition cell (B7.6 stub).
enum class CellResidencyState : u8 {
    Unloaded,
    QueuedLoad,
    Loading,
    Resident,
    QueuedUnload,
    Unloading,
};

[[nodiscard]] inline bool is_resident_state(CellResidencyState state) {
    return state == CellResidencyState::Resident;
}

[[nodiscard]] inline bool is_loading_state(CellResidencyState state) {
    return state == CellResidencyState::QueuedLoad || state == CellResidencyState::Loading;
}

[[nodiscard]] inline bool is_unloading_state(CellResidencyState state) {
    return state == CellResidencyState::QueuedUnload || state == CellResidencyState::Unloading;
}

[[nodiscard]] inline bool is_transitional_state(CellResidencyState state) {
    return is_loading_state(state) || is_unloading_state(state);
}

[[nodiscard]] inline bool is_queued_state(CellResidencyState state) {
    return state == CellResidencyState::QueuedLoad || state == CellResidencyState::QueuedUnload;
}

/// One spatial cell in the world partition grid.
struct WorldCell {
    GridCoord coord{};
    fuse::spatial::AABB world_bounds{};
    std::string asset_path;
    CellResidencyState residency = CellResidencyState::Unloaded;
    bool visible = false;
    f32 load_priority = 0.f;
    f32 unload_priority = 0.f;
    std::vector<fuse::ecs::EntityID> entities;
};

} // namespace fuse::world_partition
