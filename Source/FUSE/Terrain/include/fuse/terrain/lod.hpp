#pragma once

#include <fuse/terrain/math.hpp>
#include <fuse/terrain/terrain_desc.hpp>
#include <fuse/types.hpp>

namespace fuse::terrain {

/// Discrete LOD ring for clipmap / chunk streaming (B7.5 stub).
struct LodLevel {
    u32 level = 0;
    f32 world_stride = 1.f;
    u32 texel_step = 1;
};

/// Select LOD index from camera distance to chunk centre (stub heuristic).
[[nodiscard]] u32 select_lod_level(f32 distance_to_chunk, u32 max_lod_levels, f32 lod_bias = 1.f);

/// Build LOD table for a terrain description.
[[nodiscard]] LodLevel make_lod_level(const TerrainDesc& desc, u32 level);

} // namespace fuse::terrain
