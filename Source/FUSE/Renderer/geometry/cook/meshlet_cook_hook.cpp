#include <fuse/renderer/geometry/meshlet_cook_hook.hpp>

#include <fstream>
#include <iterator>

namespace fuse::renderer::geometry {

namespace {

bool read_bytes(const std::string& path, std::vector<u8>& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

void set_error(std::string* error, const std::string& message) {
    if (error != nullptr) {
        *error = message;
    }
}

} // namespace

bool build_meshlets_from_cooked(const cook::CookedMesh& mesh, const MeshletBuildOptions& options, MeshletMesh& out,
                                std::string* error) {
    const u32 vertexCount = mesh.vertex_count();
    if (mesh.normals.size() != static_cast<usize>(vertexCount) * 3u || mesh.uvs.size() != static_cast<usize>(vertexCount) * 2u) {
        set_error(error, "meshlet cook: FMSH streams disagree with the vertex count");
        return false;
    }
    MeshletSource source;
    source.positions = mesh.positions.data();
    source.normals = mesh.normals.data();
    source.uvs = mesh.uvs.data();
    source.vertex_count = vertexCount;
    source.indices = mesh.indices.data();
    source.index_count = static_cast<u32>(mesh.indices.size());
    for (const cook::CookedMesh::Submesh& sub : mesh.submeshes) {
        source.submeshes.push_back({sub.index_offset, sub.index_count, sub.material_index});
    }
    return build_meshlets(source, options, out, error);
}

bool meshlet_cook_post_hook(const cook::CookedMesh& mesh, const std::vector<u8>& fmsh_bytes, const std::string& output_path,
                            std::string* note) {
    MeshletBuildOptions options;
    options.source_hash = meshlet_fnv1a64(fmsh_bytes.data(), fmsh_bytes.size());
    MeshletMesh built;
    if (!build_meshlets_from_cooked(mesh, options, built, note)) {
        return false;
    }
    const std::string sidecar = meshlet_sidecar_path(output_path);
    if (!write_meshlet_file(sidecar, built, note)) {
        return false;
    }
    set_error(note, "meshlets=" + std::to_string(built.meshlets.size()) + " sidecar=" + sidecar);
    return true;
}

cook::MeshCookOptions with_meshlet_sidecar(cook::MeshCookOptions options) {
    options.post_hook = &meshlet_cook_post_hook;
    return options;
}

bool cook_meshlet_sidecar(const std::string& fusemesh_path, std::string* error, MeshletMesh* built) {
    std::vector<u8> bytes;
    if (!read_bytes(fusemesh_path, bytes)) {
        set_error(error, "meshlet cook: cannot read " + fusemesh_path);
        return false;
    }
    cook::CookedMesh mesh;
    std::string why;
    if (!cook::deserialize_cooked_mesh(bytes.data(), bytes.size(), mesh, &why)) {
        set_error(error, "meshlet cook: " + fusemesh_path + ": " + why);
        return false;
    }
    MeshletBuildOptions options;
    options.source_hash = meshlet_fnv1a64(bytes.data(), bytes.size());
    MeshletMesh local;
    MeshletMesh& target = built != nullptr ? *built : local;
    if (!build_meshlets_from_cooked(mesh, options, target, error)) {
        return false;
    }
    return write_meshlet_file(meshlet_sidecar_path(fusemesh_path), target, error);
}

bool meshlet_sidecar_matches(const std::string& fusemesh_path, std::string* error) {
    std::vector<u8> bytes;
    if (!read_bytes(fusemesh_path, bytes)) {
        set_error(error, "meshlet cook: cannot read " + fusemesh_path);
        return false;
    }
    MeshletMesh sidecar;
    if (!load_meshlet_file(meshlet_sidecar_path(fusemesh_path), sidecar, error)) {
        return false;
    }
    if (sidecar.source_hash != meshlet_fnv1a64(bytes.data(), bytes.size())) {
        set_error(error, "meshlet cook: sidecar is stale (built from other FMSH bytes)");
        return false;
    }
    return true;
}

} // namespace fuse::renderer::geometry
