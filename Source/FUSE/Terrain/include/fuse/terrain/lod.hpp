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

/// Fine/coarse LOD pair used when morphing between adjacent clipmap rings.
struct AdjacentLodPair {
    u32 fine_lod = 0;
    u32 coarse_lod = 0;
    f32 morph_factor = 0.f; // [0,1] blend toward coarse_lod vertex grid
};

/// CPU stub vertex budget for a displaced chunk mesh (surface grid + skirts + seams).
struct LodMeshVertexCounts {
    u32 grid_vertices = 0;
    u32 skirt_vertices = 0;
    u32 seam_vertices = 0;
    u32 total_vertices = 0;
};

/// Select LOD index from camera distance to chunk centre (stub heuristic).
[[nodiscard]] u32 select_lod_level(f32 distance_to_chunk, u32 max_lod_levels, f32 lod_bias = 1.f);

/// Compute LOD level and morph factor for clipmap ring transitions.
[[nodiscard]] LodTransition compute_lod_transition(f32 distance_to_chunk, u32 max_lod_levels, f32 lod_bias = 1.f);

/// Clamp morph factor to [0, 1].
[[nodiscard]] f32 clamp_morph_factor(f32 morph_factor);

/// Build the adjacent LOD pair for a clipmap transition (fine = transition.lod, coarse = fine + 1).
[[nodiscard]] AdjacentLodPair make_adjacent_lod_pair(const LodTransition& transition, u32 max_lod_levels);

/// Blend vertex XZ between fine and coarse LOD grids using an adjacent LOD pair.
[[nodiscard]] vec3 blend_morph_between_lods(vec3 position, const AdjacentLodPair& pair, f32 base_stride);

/// Morph a vertex XZ position toward the coarser LOD grid (CPU stub for vertex-shader displacement).
[[nodiscard]] vec3 morph_vertex_position(vec3 position, u32 lod, f32 morph_factor, f32 base_stride);

/// Count displaced-grid, skirt, and seam vertices for a chunk at the given LOD (CPU stub).
[[nodiscard]] LodMeshVertexCounts compute_lod_mesh_vertex_counts(u32 chunk_resolution, u32 lod, u32 max_lod_levels,
                                                                 bool include_skirts = true,
                                                                 u32 seam_neighbor_lod_delta = 0);

/// Build LOD table for a terrain description.
[[nodiscard]] LodLevel make_lod_level(const TerrainDesc& desc, u32 level);

} // namespace fuse::terrain
