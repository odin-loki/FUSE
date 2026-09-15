#pragma once

#include <fuse/terrain/math.hpp>
#include <fuse/types.hpp>

namespace fuse::terrain {

/// Heightfield + optional underground SVO configuration (B7.5).
struct TerrainDesc {
    u32 resolution = 256;
    f32 world_size = 256.f;
    f32 max_height = 64.f;
    u32 lod_levels = 4;
    u32 chunk_resolution = 64;
    bool has_svo_caves = true;
    u32 svo_depth = 8;
};

/// One terrain chunk in the clipmap / chunk grid (GPU handles stubbed until B2/B3 integration).
struct TerrainChunk {
    u32 lod = 0;
    f32 morph_factor = 0.f; // clipmap ring morph blend [0,1]
    ivec2 chunk_coord{};
    AABB world_bounds{};
    u64 vertex_buffer = 0;
    u64 index_buffer = 0;
    u32 index_count = 0;
    bool dirty = false;
    bool loaded = false;
};

} // namespace fuse::terrain
