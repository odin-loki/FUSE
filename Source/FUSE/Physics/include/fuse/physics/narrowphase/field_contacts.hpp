#pragma once

// Sphere / box / capsule vs voxel volumes and signed-distance fields (UNI-B4-VOX-1).
//
//   Voxel  the shape's bounds pick the voxels (one SVO read per brick, VoxelVolume storage == the Scene
//          SVO); every solid surface voxel is an axis-aligned box and gives the primitive box contacts.
//          A contact whose normal points into a solid neighbour (an internal voxel face, the seam between
//          two floor voxels) takes the voxel's occupancy-gradient normal instead, with the depth measured
//          along it, so bodies slide over voxel floors without catching on seams.
//   SdfMesh the field is sampled through the generic SdfSampler interface (analytic fields, SVO distance,
//          later A-SDF bricks): the sphere centre, capsule ends and deepest axis point, or the box's corners,
//          edge midpoints and face centres; normals are the normalised field gradient.
// Contacts are merged per body pair by ContactClusterer (one manifold per normal cluster).

#include <fuse/physics/math.hpp>
#include <fuse/physics/narrowphase/collision_dispatch.hpp>
#include <fuse/types.hpp>

namespace fuse::physics {
class VoxelVolume;
class SdfSampler;
} // namespace fuse::physics

namespace fuse::physics::narrowphase {

/// A voxel volume or SDF at a pose (exactly one of the two is set).
struct FieldPose {
    const VoxelVolume* voxel = nullptr;
    const SdfSampler* sdf = nullptr;
    vec3 position{};
    quat orientation{};
};

/// Sphere / box / capsule (A) vs field (B). Returns the manifold count written to `out`.
u32 collideConvexField(const ShapeInstance& convex, const FieldPose& field, u32 idxA, u32 idxB, f32 margin,
                       ContactManifold* out, u32 maxOut);

} // namespace fuse::physics::narrowphase
