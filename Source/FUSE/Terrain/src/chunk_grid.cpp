#include <fuse/terrain/chunk_grid.hpp>

#include <algorithm>
#include <cmath>

namespace fuse::terrain {

void ChunkGrid::init(const TerrainDesc& desc) {
    destroy();
    m_desc = desc;
    m_lod_levels.clear();
    for (u32 level = 0; level < desc.lod_levels; ++level) {
        m_lod_levels.push_back(make_lod_level(desc, level));
    }

    m_chunks_per_axis = std::max(1u, desc.resolution / std::max(desc.chunk_resolution, 1u));
    rebuild_chunks();
    m_initialized = true;
}

void ChunkGrid::destroy() {
    m_chunks.clear();
    m_lod_levels.clear();
    m_initialized = false;
}

ivec2 ChunkGrid::world_to_chunk_coord(f32 world_x, f32 world_z) const {
    const f32 chunk_size = m_desc.world_size / static_cast<f32>(m_chunks_per_axis);
    const s32 x = static_cast<s32>(std::floor(world_x / chunk_size));
    const s32 z = static_cast<s32>(std::floor(world_z / chunk_size));
    return {x, z};
}

AABB ChunkGrid::chunk_world_bounds(ivec2 coord, u32 lod) const {
    const LodLevel& level = m_lod_levels[std::min(lod, static_cast<u32>(m_lod_levels.size()) - 1)];
    const f32 chunk_size = m_desc.world_size / static_cast<f32>(m_chunks_per_axis) * level.world_stride;

    const f32 min_x = static_cast<f32>(coord.x) * chunk_size;
    const f32 min_z = static_cast<f32>(coord.y) * chunk_size;
    return {
        {min_x, 0.f, min_z},
        {min_x + chunk_size, m_desc.max_height, min_z + chunk_size},
    };
}

void ChunkGrid::update_lod(vec3 camera_pos, f32 /*dt*/) {
    if (!m_initialized) {
        return;
    }

    for (TerrainChunk& chunk : m_chunks) {
        const vec3 centre = chunk.world_bounds.center();
        const f32 dx = centre.x - camera_pos.x;
        const f32 dz = centre.z - camera_pos.z;
        const f32 distance = std::sqrt(dx * dx + dz * dz);

        const u32 new_lod = select_lod_level(distance, m_desc.lod_levels);
        chunk.lod = new_lod;
        chunk.world_bounds = chunk_world_bounds(chunk.chunk_coord, new_lod);
        const f32 load_radius = m_desc.world_size * 0.75f;
        chunk.loaded = distance < load_radius;
        chunk.dirty = chunk.loaded;
    }
}

u32 ChunkGrid::visible_chunk_count() const {
    u32 count = 0;
    for (const TerrainChunk& chunk : m_chunks) {
        if (chunk.loaded) {
            ++count;
        }
    }
    return count;
}

void ChunkGrid::collect_visible_chunks(std::vector<const TerrainChunk*>& out) const {
    out.clear();
    for (const TerrainChunk& chunk : m_chunks) {
        if (chunk.loaded) {
            out.push_back(&chunk);
        }
    }
}

void ChunkGrid::rebuild_chunks() {
    m_chunks.clear();
    m_chunks.reserve(static_cast<usize>(m_chunks_per_axis) * static_cast<usize>(m_chunks_per_axis));

    for (u32 z = 0; z < m_chunks_per_axis; ++z) {
        for (u32 x = 0; x < m_chunks_per_axis; ++x) {
            TerrainChunk chunk{};
            chunk.chunk_coord = {static_cast<s32>(x), static_cast<s32>(z)};
            chunk.lod = 0;
            chunk.world_bounds = chunk_world_bounds(chunk.chunk_coord, chunk.lod);
            chunk.loaded = false;
            m_chunks.push_back(chunk);
        }
    }
}

} // namespace fuse::terrain
