#pragma once

// Collision cook (GAP-PHYS-HULL-MESH part 2): FMSH -> `.fusecol` (fuse/physics/assets/collision_asset.hpp).
//   hulls  quickhull (fuse::physics::buildConvexHull) over the mesh positions, or one hull per submesh
//          (a convex decomposition by parts: artists split props into convex pieces as submeshes)
//   mesh   the static triangle mesh (all submeshes) with its BVH (binned SAH) and internal-edge flags
// `fuse_cook --collision --input <mesh.fusemesh> --output <mesh.fusecol> [--collision-mode hull|mesh|both]
// [--hull-per-submesh]`. The output bytes depend only on the mesh and the options (deterministic).

#include <fuse/asset/cooked_mesh.hpp>
#include <fuse/cook/cook_stub_writer.hpp>
#include <fuse/physics/assets/collision_asset.hpp>
#include <fuse/types.hpp>

#include <string>

namespace fuse::cook {

enum class CollisionCookMode : u32 {
    Hull = 0,    ///< convex hull(s) only (props, dynamic bodies)
    TriMesh = 1, ///< static triangle mesh only (level geometry)
    Both = 2,
};

struct CollisionCookOptions {
    CollisionCookMode mode = CollisionCookMode::Both;
    /// One hull per submesh instead of one hull over the whole mesh.
    bool hull_per_submesh = false;
    /// Quickhull distance tolerance (0: automatic, 1e-5 of the extent).
    f32 hull_tolerance = 0.f;
};

struct CollisionCookReport {
    u32 hull_count = 0;
    u32 hull_vertices = 0;
    u32 hull_faces = 0;
    u32 mesh_triangles = 0;
    u32 bvh_nodes = 0;
    u32 bvh_depth = 0;
    u32 bytes = 0;
};

[[nodiscard]] const char* collision_cook_mode_name(CollisionCookMode mode);
/// "hull" / "mesh" / "both" (false for anything else).
bool parse_collision_cook_mode(const std::string& text, CollisionCookMode& out);

/// Builds the collision asset of a cooked mesh. False (with `error`) for a mesh without triangles, or a
/// hull request whose points are flat / collinear.
bool cook_collision(const asset::CookedMesh& mesh, const CollisionCookOptions& options, physics::CollisionAsset& out,
                    CollisionCookReport* report = nullptr, std::string* error = nullptr);

/// Reads an FMSH (`.fusemesh`), cooks it and writes the `.fusecol`.
CookStubWriteResult cook_collision_file(const std::string& input_path, const std::string& output_path,
                                        const CollisionCookOptions& options = {},
                                        CollisionCookReport* report = nullptr);

} // namespace fuse::cook
