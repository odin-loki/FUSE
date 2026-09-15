#include <fuse/terrain/lod.hpp>

#include <algorithm>
#include <cmath>

namespace fuse::terrain {

namespace {

constexpr f32 kLodRingWidth = 8.f;
constexpr f32 kLodMorphFraction = 0.25f;

} // namespace

u32 select_lod_level(f32 distance_to_chunk, u32 max_lod_levels, f32 lod_bias) {
    return compute_lod_transition(distance_to_chunk, max_lod_levels, lod_bias).lod;
}

LodTransition compute_lod_transition(f32 distance_to_chunk, u32 max_lod_levels, f32 lod_bias) {
    LodTransition result{};
    if (max_lod_levels == 0) {
        return result;
    }

    const f32 biased_distance = distance_to_chunk / std::max(lod_bias, 0.001f);
    const u32 level = static_cast<u32>(biased_distance / kLodRingWidth);
    result.lod = std::min(level, max_lod_levels - 1);

    if (result.lod >= max_lod_levels - 1) {
        return result;
    }

    const f32 ring_start = static_cast<f32>(result.lod) * kLodRingWidth;
    const f32 ring_end = ring_start + kLodRingWidth;
    const f32 morph_start = ring_end - kLodRingWidth * kLodMorphFraction;

    if (biased_distance >= morph_start) {
        const f32 morph_range = ring_end - morph_start;
        result.morph_factor = std::clamp((biased_distance - morph_start) / morph_range, 0.f, 1.f);
    }

    return result;
}

f32 clamp_morph_factor(f32 morph_factor) {
    return std::clamp(morph_factor, 0.f, 1.f);
}

AdjacentLodPair make_adjacent_lod_pair(const LodTransition& transition, u32 max_lod_levels) {
    AdjacentLodPair pair{};
    if (max_lod_levels == 0) {
        return pair;
    }

    pair.fine_lod = std::min(transition.lod, max_lod_levels - 1);
    pair.coarse_lod = std::min(pair.fine_lod + 1, max_lod_levels - 1);
    pair.morph_factor = clamp_morph_factor(transition.morph_factor);
    return pair;
}

AdjacentLodPair clamp_adjacent_lod_pair(const AdjacentLodPair& pair) {
    AdjacentLodPair clamped = pair;
    clamped.morph_factor = clamp_morph_factor(pair.morph_factor);
    return clamped;
}

f32 blend_adjacent_lod_morph(f32 from_morph, f32 to_morph, f32 t) {
    const f32 clamped_from = clamp_morph_factor(from_morph);
    const f32 clamped_to = clamp_morph_factor(to_morph);
    const f32 clamped_t = clamp_morph_factor(t);
    return clamped_from + (clamped_to - clamped_from) * clamped_t;
}

vec3 blend_morph_between_lods(vec3 position, const AdjacentLodPair& pair, f32 base_stride) {
    const AdjacentLodPair clamped = clamp_adjacent_lod_pair(pair);
    if (clamped.morph_factor <= 0.f || clamped.fine_lod == clamped.coarse_lod || base_stride <= 0.f) {
        return position;
    }

    const vec3 fine = morph_vertex_position(position, clamped.fine_lod, 0.f, base_stride);
    const vec3 coarse = morph_vertex_position(position, clamped.coarse_lod, 1.f, base_stride);
    const f32 t = clamped.morph_factor;

    return {fine.x + (coarse.x - fine.x) * t, position.y, fine.z + (coarse.z - fine.z) * t};
}

LodSkirtParams clamp_skirt_params(const LodSkirtParams& params, f32 max_depth) {
    LodSkirtParams clamped = params;
    clamped.depth = std::clamp(params.depth, 0.f, std::max(max_depth, 0.f));
    clamped.segments = std::max(params.segments, 1u);
    return clamped;
}

u32 compute_skirt_vertex_strip_count(u32 edge_vertex_count, u32 segments) {
    if (edge_vertex_count == 0u || segments == 0u) {
        return 0u;
    }
    return edge_vertex_count * std::max(segments, 1u);
}

LodMeshVertexCounts compute_lod_mesh_vertex_counts(u32 chunk_resolution, u32 lod, u32 max_lod_levels,
                                                   bool include_skirts, u32 seam_neighbor_lod_delta,
                                                   const LodSkirtParams& skirt_params) {
    LodMeshVertexCounts counts{};
    if (chunk_resolution == 0 || max_lod_levels == 0) {
        return counts;
    }

    const u32 clamped_lod = std::min(lod, max_lod_levels - 1);
    const u32 texel_step = static_cast<u32>(1u << std::min(clamped_lod, 31u));
    const u32 lod_cells = std::max(chunk_resolution / texel_step, 1u);
    const u32 verts_per_edge = lod_cells + 1;

    counts.grid_vertices = verts_per_edge * verts_per_edge;
    if (include_skirts) {
        const LodSkirtParams clamped_skirt = clamp_skirt_params(skirt_params, 64.f);
        counts.skirt_vertices = 4u * compute_skirt_vertex_strip_count(verts_per_edge, clamped_skirt.segments);
    }
    counts.seam_vertices =
        seam_neighbor_lod_delta > 0 ? 4u * seam_neighbor_lod_delta * verts_per_edge : 0u;
    counts.total_vertices = counts.grid_vertices + counts.skirt_vertices + counts.seam_vertices;
    return counts;
}

vec3 morph_vertex_position(vec3 position, u32 lod, f32 morph_factor, f32 base_stride) {
    if (lod == 0 || morph_factor <= 0.f || base_stride <= 0.f) {
        return position;
    }

    const f32 grid = base_stride * static_cast<f32>(1u << lod);
    const f32 snapped_x = std::round(position.x / grid) * grid;
    const f32 snapped_z = std::round(position.z / grid) * grid;
    const f32 t = std::clamp(morph_factor, 0.f, 1.f);

    return {position.x + (snapped_x - position.x) * t, position.y,
            position.z + (snapped_z - position.z) * t};
}

LodLevel make_lod_level(const TerrainDesc& desc, u32 level) {
    LodLevel lod{};
    lod.level = level;
    lod.texel_step = static_cast<u32>(1u << level);
    const f32 chunk_world_size = desc.world_size / static_cast<f32>(std::max(desc.chunk_resolution, 1u));
    lod.world_stride = chunk_world_size * static_cast<f32>(lod.texel_step);
    return lod;
}

} // namespace fuse::terrain
