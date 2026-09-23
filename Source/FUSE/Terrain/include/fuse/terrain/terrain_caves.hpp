#pragma once

#if defined(FUSE_TERRAIN_HAS_SVO) && FUSE_TERRAIN_HAS_SVO
#include <fuse/scene/svo.hpp>
#endif
#include <fuse/terrain/math.hpp>
#include <fuse/terrain/terrain_desc.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::terrain {

#if defined(FUSE_TERRAIN_HAS_SVO) && FUSE_TERRAIN_HAS_SVO

/// Underground cave volume stored in a sparse voxel octree (B7.5 over B3.5 SVO).
///
/// The SVO cube spans the terrain footprint in X/Z and reaches from `max_height` down by
/// `world_size`. Voxels written with `kCaveAirMaterial` are carved-out air; everything else below
/// the heightfield surface is solid rock. The heightfield stays the fast surface representation;
/// the SVO only answers "is this sub-surface point hollow?" for ray marching and collision.
class TerrainCaves {
public:
    static constexpr u32 kCaveAirMaterial = 1u;

    void init(const TerrainDesc& desc);
    void destroy();

    /// Mark every voxel whose centre lies within `radius` of `center` as cave air.
    void carve_sphere(vec3 center, f32 radius);

    /// True when `point` falls in a carved voxel.
    [[nodiscard]] bool is_cave(vec3 point) const;

    /// Inward-facing wall normal at `point` (towards the nearest carved sphere's centre).
    [[nodiscard]] vec3 wall_normal(vec3 point) const;

    [[nodiscard]] bool empty() const { return m_spheres.empty(); }
    [[nodiscard]] bool is_initialized() const { return m_svo.isInitialized(); }
    [[nodiscard]] f32 voxel_size() const { return m_voxel_size; }
    [[nodiscard]] usize carved_voxel_count() const { return m_svo.voxelCount(); }

private:
    struct Sphere {
        vec3 center{};
        f32 radius = 0.f;
    };

    [[nodiscard]] bool to_voxel(vec3 point, fuse::scene::ivec3& out) const;

    fuse::scene::SVO m_svo;
    vec3 m_origin{};
    f32 m_voxel_size = 1.f;
    std::vector<Sphere> m_spheres;
};

#else

/// Build without fuse_scene (FUSE_BUILD_PROJECT=OFF): no SVO, so no caves can be carved.
class TerrainCaves {
public:
    static constexpr u32 kCaveAirMaterial = 1u;

    void init(const TerrainDesc& /*desc*/) {}
    void destroy() {}
    void carve_sphere(vec3 /*center*/, f32 /*radius*/) {}
    [[nodiscard]] bool is_cave(vec3 /*point*/) const { return false; }
    [[nodiscard]] vec3 wall_normal(vec3 /*point*/) const { return {0.f, 1.f, 0.f}; }
    [[nodiscard]] bool empty() const { return true; }
    [[nodiscard]] bool is_initialized() const { return false; }
    [[nodiscard]] f32 voxel_size() const { return 1.f; }
    [[nodiscard]] usize carved_voxel_count() const { return 0; }
};

#endif

} // namespace fuse::terrain
