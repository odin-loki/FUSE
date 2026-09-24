#pragma once

#include <fuse/cook/cook_stub_writer.hpp>
#include <fuse/types.hpp>

#include <string>
#include <vector>

namespace fuse::cook {

/// Engine binary mesh (`.fusemesh`, magic `FMSH`). Triangle list with 32-bit indices; every
/// vertex carries position, normal and uv0 (zero when the source has no texture coordinates).
struct CookedMesh {
    struct Submesh {
        u32 index_offset = 0;
        u32 index_count = 0;
        u32 vertex_offset = 0;
        u32 material_index = 0;
    };

    std::vector<f32> positions; ///< xyz per vertex
    std::vector<f32> normals;   ///< xyz per vertex
    std::vector<f32> uvs;       ///< uv per vertex
    std::vector<u32> indices;   ///< global vertex indices, 3 per triangle
    std::vector<Submesh> submeshes;
    f32 bounds_min[3] = {0.f, 0.f, 0.f};
    f32 bounds_max[3] = {0.f, 0.f, 0.f};

    [[nodiscard]] u32 vertex_count() const { return static_cast<u32>(positions.size() / 3u); }
};

/// Optional post-write hook for `cook_mesh_file` (WP-1.2: the renderer's meshlet cook installs one
/// that writes a `.fusemeshlet` sidecar, see Source/FUSE/Renderer/geometry/meshlet_cook_hook.hpp).
/// Called after the `.fusemesh` is written, with the cooked mesh, its exact FMSH bytes and the output
/// path. It never changes the FMSH bytes. Returning false fails the cook (`WriteFailed`, hook note).
using MeshCookPostHook = bool (*)(const CookedMesh& mesh, const std::vector<u8>& fmsh_bytes,
                                  const std::string& output_path, std::string* note);

struct MeshCookOptions {
    bool generate_normals = true;
    MeshCookPostHook post_hook = nullptr; ///< nullptr: FMSH only (default)
};

inline constexpr u32 kCookedMeshVersion = 1;

/// Import FBX / glTF / OBJ / … through assimp into `CookedMesh`. Returns false (with `error` and a
/// `failure` class) when the library is absent (`ImporterUnavailable`), the file cannot be parsed
/// (`MalformedSource`), or the parsed geometry is unusable (`InvalidGeometry`): no triangles, face
/// indices outside the vertex range (including faces an importer silently dropped for that reason),
/// or non-finite positions / normals / uvs. Nothing is ever "repaired" — bad data is rejected.
bool import_mesh_file(const std::string& input_path, const MeshCookOptions& options, CookedMesh& out,
                      std::string* error = nullptr, CookFailure* failure = nullptr);

/// Serialize to the little-endian `FMSH` layout (header, submeshes, streams, FNV-1a trailer).
/// Output depends only on mesh contents — never on paths, time, or host.
std::vector<u8> serialize_cooked_mesh(const CookedMesh& mesh);

/// Parse and validate an `FMSH` blob (magic, version, sizes, index range, checksum).
bool deserialize_cooked_mesh(const u8* data, usize size, CookedMesh& out, std::string* error = nullptr);

/// Import + serialize + write. Fails without writing when the source cannot be imported; the
/// result's `failure` says why (see `import_mesh_file`).
CookStubWriteResult cook_mesh_file(const std::string& input_path, const std::string& output_path,
                                   const MeshCookOptions& options = {});

/// Read and validate a cooked `.fusemesh` file.
bool load_cooked_mesh(const std::string& path, CookedMesh& out, std::string* error = nullptr);

} // namespace fuse::cook
