#pragma once

#include <fuse/ecs/math/vec.hpp>
#include <fuse/types.hpp>
#include <fuse/world_partition/grid_cell.hpp>

#include <cmath>

namespace fuse::world_partition {

/// Camera-centered streaming radii — cells inside `stream_in_radius` load; outside
/// `stream_out_radius` unload (hysteresis avoids thrashing).
struct StreamingVolumeDesc {
    f32 stream_in_radius = 512.f;
    f32 stream_out_radius = 600.f;
};

/// Active streaming volume evaluated each frame against grid cells.
struct StreamingVolume {
    fuse::ecs::vec3 center{};
    StreamingVolumeDesc desc{};

    [[nodiscard]] f32 planar_distance_to(const fuse::ecs::vec3& point) const {
        const f32 dx = point.x - center.x;
        const f32 dz = point.z - center.z;
        return std::sqrt(dx * dx + dz * dz);
    }

    [[nodiscard]] bool should_load(GridCoord coord, f32 cell_size) const {
        const fuse::ecs::vec3 cell_center = grid_to_world_center(coord, cell_size);
        return planar_distance_to(cell_center) <= desc.stream_in_radius;
    }

    [[nodiscard]] bool should_unload(GridCoord coord, f32 cell_size) const {
        const fuse::ecs::vec3 cell_center = grid_to_world_center(coord, cell_size);
        return planar_distance_to(cell_center) > desc.stream_out_radius;
    }

    [[nodiscard]] f32 load_priority_for(GridCoord coord, f32 cell_size) const {
        const fuse::ecs::vec3 cell_center = grid_to_world_center(coord, cell_size);
        const f32 distance = planar_distance_to(cell_center);
        return std::max(0.f, desc.stream_in_radius - distance);
    }
};

} // namespace fuse::world_partition
