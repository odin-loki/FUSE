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
