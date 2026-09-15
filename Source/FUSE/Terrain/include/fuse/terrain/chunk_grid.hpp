#pragma once

#include <fuse/terrain/lod.hpp>
#include <fuse/terrain/terrain_desc.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::terrain {

/// Simple chunk grid (clipmap ring scaffold) for terrain streaming (B7.5).
class ChunkGrid {
public:
    void init(const TerrainDesc& desc);
    void destroy();

    [[nodiscard]] u32 chunk_count() const { return static_cast<u32>(m_chunks.size()); }
    [[nodiscard]] const TerrainChunk& chunk(u32 index) const { return m_chunks[index]; }
    [[nodiscard]] TerrainChunk& chunk(u32 index) { return m_chunks[index]; }

    [[nodiscard]] ivec2 world_to_chunk_coord(f32 world_x, f32 world_z) const;
    [[nodiscard]] AABB chunk_world_bounds(ivec2 coord, u32 lod) const;

    /// Mark chunks loaded/unloaded based on camera position (stub — no async IO).
    void update_lod(vec3 camera_pos, f32 dt);

    [[nodiscard]] u32 visible_chunk_count() const;
    void collect_visible_chunks(std::vector<const TerrainChunk*>& out) const;

    [[nodiscard]] const TerrainDesc& desc() const { return m_desc; }
    [[nodiscard]] bool is_initialized() const { return m_initialized; }

private:
    void rebuild_chunks();

    TerrainDesc m_desc{};
    std::vector<TerrainChunk> m_chunks;
    std::vector<LodLevel> m_lod_levels;
    u32 m_chunks_per_axis = 0;
    bool m_initialized = false;
};

} // namespace fuse::terrain
