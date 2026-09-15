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

/// Clipmap ring transition state — discrete LOD plus morph blend toward coarser grid.
struct LodTransition {
    u32 lod = 0;
    f32 morph_factor = 0.f; // [0,1] blend toward coarser LOD vertex positions
};

/// Select LOD index from camera distance to chunk centre (stub heuristic).
[[nodiscard]] u32 select_lod_level(f32 distance_to_chunk, u32 max_lod_levels, f32 lod_bias = 1.f);

/// Compute LOD level and morph factor for clipmap ring transitions.
[[nodiscard]] LodTransition compute_lod_transition(f32 distance_to_chunk, u32 max_lod_levels, f32 lod_bias = 1.f);

/// Morph a vertex XZ position toward the coarser LOD grid (CPU stub for vertex-shader displacement).
[[nodiscard]] vec3 morph_vertex_position(vec3 position, u32 lod, f32 morph_factor, f32 base_stride);

/// Build LOD table for a terrain description.
[[nodiscard]] LodLevel make_lod_level(const TerrainDesc& desc, u32 level);

} // namespace fuse::terrain
