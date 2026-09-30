#pragma once
// E02 procedural meshes for scenes without cooked assets: a cube, a ground plane and a UV sphere, built into WP-1.2
// meshlet meshes (geometry::build_meshlets, CpuReference backend: deterministic bytes). One submesh, material index 0
// (the instance's material row is used as is).
#include <fuse/renderer/geometry/meshlet_format.hpp>
#include <fuse/types.hpp>

#include <string>

namespace fuse::renderer::scene_renderer {

enum class ProceduralShape : u8 {
    Cube = 0, ///< [-halfExtent, halfExtent]^3, per-face normals and UVs
    Plane,    ///< xz plane at y = 0, normal +y, `size` wide, `segments` x `segments` quads, UVs tiled `uvTiles`
    Sphere,   ///< UV sphere of `radius` (`rings` x `segments`, seam duplicated)
};

struct ProceduralMeshDesc {
    ProceduralShape shape = ProceduralShape::Cube;
    f32 halfExtent = 1.f; ///< cube
    f32 size = 2.f;       ///< plane
    f32 uvTiles = 1.f;    ///< plane
    f32 radius = 1.f;     ///< sphere
    u32 segments = 16;    ///< plane subdivisions / sphere longitude segments
    u32 rings = 12;       ///< sphere latitude rings
};

/// Builds the meshlet mesh; `localMin` / `localMax` (optional) receive the exact analytic object-space AABB.
bool buildProceduralMesh(const ProceduralMeshDesc& desc, geometry::MeshletMesh& out, f32* localMin = nullptr,
                         f32* localMax = nullptr, std::string* error = nullptr);

} // namespace fuse::renderer::scene_renderer
