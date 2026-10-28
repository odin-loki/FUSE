#pragma once

#include <fuse/terrain/chunk_grid.hpp>
#include <fuse/terrain/chunk_mesh.hpp>
#include <fuse/terrain/heightfield.hpp>
#include <fuse/terrain/queries.hpp>
#include <fuse/terrain/terrain_caves.hpp>
#include <fuse/terrain/terrain_desc.hpp>
#include <fuse/terrain/terrain_edit_delta.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::terrain {

/// Terrain facade — heightfield surface + chunk LOD grid (B7.5 stub).
class Terrain {
public:
    void init(const TerrainDesc& desc);
    void destroy();

    /// Fill the heightfield with seeded fractal value noise (see `terrain_noise.hpp`) and mark all meshes stale.
    void generate(u64 seed);
    /// Raise (amount > 0) or lower texels within `radius` with linear falloff. The change is kept as a
    /// sparse delta on the generated base. Chunk meshes whose footprint the edit touches are rebuilt
    /// on the next `update_lod`.
    void deform(vec3 world_pos, f32 radius, f32 amount);

    /// Carve a spherical cave into the underground SVO layer and record it on the edit delta.
    /// The SVO write requires `TerrainDesc::has_svo_caves`; the record is kept either way.
    void carve_cave(vec3 center, f32 radius);

    /// Sparse height and cave edits. Empty when terrain is not initialized. Does not store the
    /// procedural heightfield itself.
    [[nodiscard]] std::vector<u8> save_edits() const;
    /// Rebuild the base from the blob (`generate` when the edits were captured after one, otherwise a
    /// flat zero field) and reapply the sparse delta. A malformed blob or a desc mismatch leaves this
    /// terrain unchanged and returns false.
    [[nodiscard]] bool load_edits(const u8* data, usize size);

    [[nodiscard]] u32 height_edit_count() const;
    [[nodiscard]] u32 cave_edit_count() const;
    /// Cave carve `index` in application order. False when `index` is past the recorded list.
    [[nodiscard]] bool cave_edit(u32 index, vec3& center, f32& radius) const;
    /// Solid rock test: below the heightfield surface and not inside a carved cave voxel.
    [[nodiscard]] bool is_solid(vec3 world_pos) const;

    [[nodiscard]] f32 get_height(f32 world_x, f32 world_z) const;
    [[nodiscard]] vec3 get_normal(f32 world_x, f32 world_z) const;
    /// Ray cast against the terrain. Without caves this is the heightfield march; with carved caves
    /// the march continues through surface openings into the SVO volume and stops at the first
    /// air->rock transition (heightfield surface or cave wall/floor).
    [[nodiscard]] bool ray_cast(vec3 origin, vec3 direction, f32 max_distance, vec3& hit, vec3& normal,
                              f32& distance) const;

    /// Advance chunk LOD/residency, then (re)build CPU meshes for resident chunks whose LOD, neighbour
    /// stitching or heights changed. Meshes of chunks that left residency are released.
    void update_lod(vec3 camera_pos, f32 dt);
    void get_visible_chunks(std::vector<const TerrainChunk*>& out) const;

    /// CPU mesh for a resident chunk, or nullptr when the chunk has no mesh (non-owning view).
    [[nodiscard]] const ChunkMesh* chunk_mesh(u32 chunk_index) const;
    /// Total chunk mesh builds since init (diagnostics / tests).
    [[nodiscard]] u64 mesh_build_count() const { return m_mesh_builds; }
    /// Resident chunks whose heights changed since their mesh was last built (0 after `update_lod`).
    [[nodiscard]] u32 height_dirty_chunk_count() const;

    [[nodiscard]] Heightfield& heightfield() { return m_heightfield; }
    [[nodiscard]] const Heightfield& heightfield() const { return m_heightfield; }
    [[nodiscard]] const TerrainCaves& caves() const { return m_caves; }
    [[nodiscard]] ChunkGrid& chunk_grid() { return m_chunk_grid; }
    [[nodiscard]] const ChunkGrid& chunk_grid() const { return m_chunk_grid; }
    [[nodiscard]] const TerrainDesc& desc() const { return m_desc; }
    [[nodiscard]] bool is_initialized() const { return m_initialized; }

private:
    void mark_height_dirty_(f32 min_x, f32 min_z, f32 max_x, f32 max_z);
    void refresh_chunk_meshes_();
    void bind_edit_desc_();
    void add_height_edit_(u32 x, u32 z, f32 delta);
    void clear_cave_volume_();
    void apply_edit_delta_(TerrainEditDelta delta);

    TerrainDesc m_desc{};
    Heightfield m_heightfield;
    ChunkGrid m_chunk_grid;
    TerrainCaves m_caves;
    TerrainEditDelta m_edits{};
    std::vector<ChunkMesh> m_meshes;
    std::vector<u8> m_height_dirty;
    u64 m_mesh_builds = 0;
    bool m_initialized = false;
};

} // namespace fuse::terrain
