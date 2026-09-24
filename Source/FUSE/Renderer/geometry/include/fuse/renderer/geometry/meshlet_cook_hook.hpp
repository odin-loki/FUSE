#pragma once

// WP-1.2 cook hook: builds the `.fusemeshlet` sidecar next to a cooked `.fusemesh` (FMSH v1).
// FMSH v1 is untouched: the hook runs after the `.fusemesh` is written, reads nothing back from it
// and writes a separate file, so every FMSH v1 reader keeps working unchanged. The sidecar records
// the FNV-1a 64 of the FMSH bytes it was built from (header `source_hash`), so a stale sidecar
// (FMSH re-cooked, sidecar not) is detectable with `meshlet_sidecar_matches`.
//
// Library fuse_geometry_cook (links fuse_cook_stubs); the core builder (fuse_geometry) has no cook
// dependency.

#include <fuse/cook/mesh_cook.hpp>
#include <fuse/renderer/geometry/meshlet_builder.hpp>

#include <string>
#include <vector>

namespace fuse::renderer::geometry {

/// Build meshlets from an FMSH v1 mesh (positions, normals, uv0, submeshes with material indices).
bool build_meshlets_from_cooked(const cook::CookedMesh& mesh, const MeshletBuildOptions& options, MeshletMesh& out,
                                std::string* error = nullptr);

/// `cook::MeshCookPostHook`: builds with default options (64 v / 124 t), `source_hash` = FNV-1a of
/// `fmsh_bytes`, and writes `meshlet_sidecar_path(output_path)`.
bool meshlet_cook_post_hook(const cook::CookedMesh& mesh, const std::vector<u8>& fmsh_bytes,
                            const std::string& output_path, std::string* note);

/// `options` with the meshlet sidecar hook installed.
[[nodiscard]] cook::MeshCookOptions with_meshlet_sidecar(cook::MeshCookOptions options = {});

/// Emit (or refresh) the sidecar for an existing `.fusemesh` file.
bool cook_meshlet_sidecar(const std::string& fusemesh_path, std::string* error = nullptr, MeshletMesh* built = nullptr);

/// True when the sidecar at `meshlet_sidecar_path(fusemesh_path)` parses and was built from the
/// current bytes of `fusemesh_path`.
bool meshlet_sidecar_matches(const std::string& fusemesh_path, std::string* error = nullptr);

} // namespace fuse::renderer::geometry
