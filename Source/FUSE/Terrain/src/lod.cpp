#include <fuse/terrain/lod.hpp>

#include <algorithm>
#include <cmath>

namespace fuse::terrain {

u32 select_lod_level(f32 distance_to_chunk, u32 max_lod_levels, f32 lod_bias) {
    if (max_lod_levels == 0) {
        return 0;
    }

    const f32 biased_distance = distance_to_chunk / std::max(lod_bias, 0.001f);
    const f32 lod_distance = 8.f;
    const u32 level = static_cast<u32>(biased_distance / lod_distance);
    return std::min(level, max_lod_levels - 1);
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
