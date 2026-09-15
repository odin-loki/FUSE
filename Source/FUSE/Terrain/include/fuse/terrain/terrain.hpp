#pragma once

#include <fuse/terrain/chunk_grid.hpp>
#include <fuse/terrain/heightfield.hpp>
#include <fuse/terrain/queries.hpp>
#include <fuse/terrain/terrain_desc.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::terrain {

/// Terrain facade — heightfield surface + chunk LOD grid (B7.5 stub).
class Terrain {
public:
    void init(const TerrainDesc& desc);
    void destroy();

    void generate(u64 seed);
    void deform(vec3 world_pos, f32 radius, f32 amount);

    [[nodiscard]] f32 get_height(f32 world_x, f32 world_z) const;
    [[nodiscard]] vec3 get_normal(f32 world_x, f32 world_z) const;
    [[nodiscard]] bool ray_cast(vec3 origin, vec3 direction, f32 max_distance, vec3& hit, vec3& normal,
                              f32& distance) const;

    void update_lod(vec3 camera_pos, f32 dt);
    void get_visible_chunks(std::vector<const TerrainChunk*>& out) const;

    [[nodiscard]] Heightfield& heightfield() { return m_heightfield; }
    [[nodiscard]] const Heightfield& heightfield() const { return m_heightfield; }
    [[nodiscard]] ChunkGrid& chunk_grid() { return m_chunk_grid; }
    [[nodiscard]] const ChunkGrid& chunk_grid() const { return m_chunk_grid; }
    [[nodiscard]] const TerrainDesc& desc() const { return m_desc; }
    [[nodiscard]] bool is_initialized() const { return m_initialized; }

private:
    TerrainDesc m_desc{};
    Heightfield m_heightfield;
    ChunkGrid m_chunk_grid;
    bool m_initialized = false;
};

} // namespace fuse::terrain
