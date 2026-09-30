#pragma once

#include <fuse/physics/math.hpp>
#include <fuse/scene/svo.hpp>

#include <vector>

namespace fuse::physics {

/// A solid piece of a voxel volume (world-space centre of mass, voxel-space bounds).
struct VoxelFragment {
    std::vector<ivec3> voxels;
    ivec3 min{};
    ivec3 max{};
    vec3 centerOfMass{};
};

/// Destructible voxel volume (B4.7). Voxel (x, y, z) covers
/// [origin + (x, y, z) * size, origin + (x + 1, y + 1, z + 1) * size); material 0 is empty.
/// Layer y == 0 is the anchored layer (resting on / fixed to the world) used to decide which
/// pieces fall off after a carve.
///
/// UNI-B4-VOX-1: the voxels are stored in the Scene sparse voxel octree (fuse::scene::SVO, the one SVO
/// type shared by Scene and Physics, with its packed depth-first ray layout): SVO voxel (x, y, z) is this
/// volume's voxel (x, y, z), the SVO origin is `origin` and its leaf size is `voxelSize`. The volume only
/// adds the box `dims` (voxels outside it read as empty) and the physics-side algorithms (carve by voxel
/// centre, dual contouring, floating-piece detach). As a Voxel collision shape (shape pool) contacts
/// read the same storage, so a carve refreshes the collision shape in place.
class VoxelVolume {
public:
    void init(vec3 origin, f32 voxelSize, ivec3 dims);

    [[nodiscard]] bool inBounds(ivec3 v) const;
    [[nodiscard]] u8 get(ivec3 v) const; ///< 0 outside the volume
    void set(ivec3 v, u8 material);
    /// Inclusive box fill.
    void fill(ivec3 minCorner, ivec3 maxCorner, u8 material);
    /// Removes every voxel whose centre lies inside the sphere; returns how many were removed.
    u32 carve(vec3 center, f32 radius);

    [[nodiscard]] u32 solidCount() const { return static_cast<u32>(m_svo.voxelCount()); }
    [[nodiscard]] vec3 voxelCenter(ivec3 v) const;
    [[nodiscard]] ivec3 voxelAt(vec3 world) const;
    [[nodiscard]] f32 voxelSize() const { return m_size; }
    [[nodiscard]] ivec3 dims() const { return m_dims; }
    [[nodiscard]] vec3 origin() const { return m_origin; }
    /// The shared octree storage.
    [[nodiscard]] const scene::SVO& svo() const { return m_svo; }
    /// Outward occupancy-gradient normal at voxel `v` (from its 26 neighbours; zero when the
    /// neighbourhood is symmetric, e.g. deep inside).
    [[nodiscard]] vec3 occupancyNormal(ivec3 v) const;
    /// True when any voxel overlapping the box [lo, hi] (volume-space world units) is solid.
    [[nodiscard]] bool anySolidIn(vec3 lo, vec3 hi) const;

    /// Dual contouring of the occupancy field: one vertex per boundary cell of the dual grid
    /// (placed at the mass point of its sign-changing edges) and one quad per sign-changing voxel
    /// edge. Space outside the volume counts as empty, so every surface is closed (watertight):
    /// each directed mesh edge is matched by its reverse. Triangles wind counter-clockwise seen
    /// from outside the solid.
    void extractSurface(std::vector<vec3>& vertices, std::vector<u32>& indices) const;

    /// Removes and returns every 6-connected solid piece that no longer reaches layer y == 0.
    std::vector<VoxelFragment> detachFloating();

    /// Every voxel of the box, x fastest (one octree walk per brick).
    void snapshot(std::vector<u8>& grid) const;

private:
    [[nodiscard]] usize linear(ivec3 v) const;

    vec3 m_origin{};
    f32 m_size = 1.f;
    ivec3 m_dims{};
    scene::SVO m_svo;
};

} // namespace fuse::physics
