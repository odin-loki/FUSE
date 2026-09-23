#include <fuse/cook/mesh_cook.hpp>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>

#if defined(FUSE_HAS_ASSIMP)
#include <assimp/Importer.hpp>
#include <assimp/postprocess.h>
#include <assimp/scene.h>
#endif

namespace fuse::cook {

namespace {

constexpr u8 kMagic[4] = {'F', 'M', 'S', 'H'};
constexpr usize kHeaderBytes = 4u + 5u * 4u + 6u * 4u;
constexpr usize kSubmeshBytes = 4u * 4u;
constexpr usize kTrailerBytes = 8u;

void set_error(std::string* error, const std::string& message) {
    if (error != nullptr) {
        *error = message;
    }
}

u64 fnv1a64(const u8* data, usize size) {
    u64 hash = 14695981039346656037ull;
    for (usize i = 0; i < size; ++i) {
        hash ^= static_cast<u64>(data[i]);
        hash *= 1099511628211ull;
    }
    return hash;
}

void put_u32(std::vector<u8>& out, u32 value) {
    for (u32 shift = 0; shift < 32u; shift += 8u) {
        out.push_back(static_cast<u8>((value >> shift) & 0xFFu));
    }
}

void put_u64(std::vector<u8>& out, u64 value) {
    for (u32 shift = 0; shift < 64u; shift += 8u) {
        out.push_back(static_cast<u8>((value >> shift) & 0xFFu));
    }
}

void put_f32(std::vector<u8>& out, f32 value) {
    u32 bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    put_u32(out, bits);
}

u32 get_u32(const u8* data) {
    return static_cast<u32>(data[0]) | (static_cast<u32>(data[1]) << 8) | (static_cast<u32>(data[2]) << 16) |
           (static_cast<u32>(data[3]) << 24);
}

u64 get_u64(const u8* data) {
    return static_cast<u64>(get_u32(data)) | (static_cast<u64>(get_u32(data + 4)) << 32);
}

f32 get_f32(const u8* data) {
    const u32 bits = get_u32(data);
    f32 value = 0.f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

void compute_bounds(CookedMesh& mesh) {
    const u32 count = mesh.vertex_count();
    for (u32 axis = 0; axis < 3u; ++axis) {
        mesh.bounds_min[axis] = count > 0u ? mesh.positions[axis] : 0.f;
        mesh.bounds_max[axis] = mesh.bounds_min[axis];
    }
    for (u32 v = 0; v < count; ++v) {
        for (u32 axis = 0; axis < 3u; ++axis) {
            const f32 value = mesh.positions[v * 3u + axis];
            mesh.bounds_min[axis] = std::min(mesh.bounds_min[axis], value);
            mesh.bounds_max[axis] = std::max(mesh.bounds_max[axis], value);
        }
    }
}

} // namespace

bool import_mesh_file(const std::string& input_path, const MeshCookOptions& options, CookedMesh& out,
                      std::string* error) {
    out = CookedMesh{};
#if defined(FUSE_HAS_ASSIMP)
    if (input_path.empty()) {
        set_error(error, "missing input path");
        return false;
    }
    std::error_code ec;
    if (!std::filesystem::is_regular_file(std::filesystem::path(input_path), ec)) {
        set_error(error, "source file not found");
        return false;
    }

    // Single-threaded, fixed post-process set: identical bytes in → identical mesh out.
    unsigned int flags = aiProcess_Triangulate | aiProcess_JoinIdenticalVertices | aiProcess_SortByPType |
                         aiProcess_PreTransformVertices | aiProcess_ValidateDataStructure;
    if (options.generate_normals) {
        flags |= aiProcess_GenSmoothNormals;
    }

    Assimp::Importer importer;
    const aiScene* scene = importer.ReadFile(input_path, flags);
    if (scene == nullptr) {
        set_error(error, std::string("mesh import failed: ") + importer.GetErrorString());
        return false;
    }

    for (u32 meshIndex = 0; meshIndex < scene->mNumMeshes; ++meshIndex) {
        const aiMesh* mesh = scene->mMeshes[meshIndex];
        if (mesh == nullptr || (mesh->mPrimitiveTypes & aiPrimitiveType_TRIANGLE) == 0u || mesh->mNumVertices == 0u) {
            continue;
        }
        CookedMesh::Submesh submesh;
        submesh.vertex_offset = out.vertex_count();
        submesh.index_offset = static_cast<u32>(out.indices.size());
        submesh.material_index = mesh->mMaterialIndex;

        for (u32 v = 0; v < mesh->mNumVertices; ++v) {
            const aiVector3D& p = mesh->mVertices[v];
            out.positions.insert(out.positions.end(), {p.x, p.y, p.z});
            if (mesh->HasNormals()) {
                const aiVector3D& n = mesh->mNormals[v];
                out.normals.insert(out.normals.end(), {n.x, n.y, n.z});
            } else {
                out.normals.insert(out.normals.end(), {0.f, 0.f, 0.f});
            }
            if (mesh->HasTextureCoords(0)) {
                const aiVector3D& t = mesh->mTextureCoords[0][v];
                out.uvs.insert(out.uvs.end(), {t.x, t.y});
            } else {
                out.uvs.insert(out.uvs.end(), {0.f, 0.f});
            }
        }
        for (u32 f = 0; f < mesh->mNumFaces; ++f) {
            const aiFace& face = mesh->mFaces[f];
            if (face.mNumIndices != 3u) {
                continue;
            }
            for (u32 corner = 0; corner < 3u; ++corner) {
                out.indices.push_back(submesh.vertex_offset + face.mIndices[corner]);
            }
        }
        submesh.index_count = static_cast<u32>(out.indices.size()) - submesh.index_offset;
        if (submesh.index_count > 0u) {
            out.submeshes.push_back(submesh);
        }
    }

    if (out.indices.empty()) {
        set_error(error, "mesh import produced no triangles");
        out = CookedMesh{};
        return false;
    }
    compute_bounds(out);
    return true;
#else
    (void)input_path;
    (void)options;
    set_error(error, "assimp unavailable (library not linked)");
    return false;
#endif
}

std::vector<u8> serialize_cooked_mesh(const CookedMesh& mesh) {
    const u32 vertexCount = mesh.vertex_count();
    std::vector<u8> out;
    out.reserve(kHeaderBytes + mesh.submeshes.size() * kSubmeshBytes + vertexCount * 32u +
                mesh.indices.size() * 4u + kTrailerBytes);
    out.insert(out.end(), std::begin(kMagic), std::end(kMagic));
    put_u32(out, kCookedMeshVersion);
    put_u32(out, 0u); // flags (reserved)
    put_u32(out, vertexCount);
    put_u32(out, static_cast<u32>(mesh.indices.size()));
    put_u32(out, static_cast<u32>(mesh.submeshes.size()));
    for (f32 value : mesh.bounds_min) {
        put_f32(out, value);
    }
    for (f32 value : mesh.bounds_max) {
        put_f32(out, value);
    }
    for (const CookedMesh::Submesh& submesh : mesh.submeshes) {
        put_u32(out, submesh.index_offset);
        put_u32(out, submesh.index_count);
        put_u32(out, submesh.vertex_offset);
        put_u32(out, submesh.material_index);
    }
    for (u32 v = 0; v < vertexCount * 3u; ++v) {
        put_f32(out, mesh.positions[v]);
    }
    for (u32 v = 0; v < vertexCount * 3u; ++v) {
        put_f32(out, v < mesh.normals.size() ? mesh.normals[v] : 0.f);
    }
    for (u32 v = 0; v < vertexCount * 2u; ++v) {
        put_f32(out, v < mesh.uvs.size() ? mesh.uvs[v] : 0.f);
    }
    for (u32 index : mesh.indices) {
        put_u32(out, index);
    }
    put_u64(out, fnv1a64(out.data(), out.size()));
    return out;
}

bool deserialize_cooked_mesh(const u8* data, usize size, CookedMesh& out, std::string* error) {
    out = CookedMesh{};
    if (data == nullptr || size < kHeaderBytes + kTrailerBytes) {
        set_error(error, "cooked mesh truncated");
        return false;
    }
    if (std::memcmp(data, kMagic, sizeof(kMagic)) != 0) {
        set_error(error, "cooked mesh magic mismatch");
        return false;
    }
    const u32 version = get_u32(data + 4);
    if (version != kCookedMeshVersion) {
        set_error(error, "cooked mesh version " + std::to_string(version) + " unsupported");
        return false;
    }
    const u32 vertexCount = get_u32(data + 12);
    const u32 indexCount = get_u32(data + 16);
    const u32 submeshCount = get_u32(data + 20);
    const u64 expected = kHeaderBytes + static_cast<u64>(submeshCount) * kSubmeshBytes +
                         static_cast<u64>(vertexCount) * 32u + static_cast<u64>(indexCount) * 4u + kTrailerBytes;
    if (expected != size) {
        set_error(error, "cooked mesh size mismatch");
        return false;
    }
    if (get_u64(data + size - kTrailerBytes) != fnv1a64(data, size - kTrailerBytes)) {
        set_error(error, "cooked mesh checksum mismatch");
        return false;
    }

    const u8* cursor = data + 24;
    for (f32& value : out.bounds_min) {
        value = get_f32(cursor);
        cursor += 4;
    }
    for (f32& value : out.bounds_max) {
        value = get_f32(cursor);
        cursor += 4;
    }
    out.submeshes.resize(submeshCount);
    for (CookedMesh::Submesh& submesh : out.submeshes) {
        submesh.index_offset = get_u32(cursor);
        submesh.index_count = get_u32(cursor + 4);
        submesh.vertex_offset = get_u32(cursor + 8);
        submesh.material_index = get_u32(cursor + 12);
        cursor += kSubmeshBytes;
        if (static_cast<u64>(submesh.index_offset) + submesh.index_count > indexCount) {
            set_error(error, "cooked mesh submesh range out of bounds");
            out = CookedMesh{};
            return false;
        }
    }
    auto read_floats = [&](std::vector<f32>& dst, usize count) {
        dst.resize(count);
        for (f32& value : dst) {
            value = get_f32(cursor);
            cursor += 4;
        }
    };
    read_floats(out.positions, static_cast<usize>(vertexCount) * 3u);
    read_floats(out.normals, static_cast<usize>(vertexCount) * 3u);
    read_floats(out.uvs, static_cast<usize>(vertexCount) * 2u);
    out.indices.resize(indexCount);
    for (u32& index : out.indices) {
        index = get_u32(cursor);
        cursor += 4;
        if (index >= vertexCount) {
            set_error(error, "cooked mesh index out of range");
            out = CookedMesh{};
            return false;
        }
    }
    return true;
}

CookStubWriteResult cook_mesh_file(const std::string& input_path, const std::string& output_path,
                                   const MeshCookOptions& options) {
    CookStubWriteResult result;
    if (input_path.empty() || output_path.empty()) {
        result.note = "missing input or output path";
        return result;
    }
    CookedMesh mesh;
    std::string error;
    if (!import_mesh_file(input_path, options, mesh, &error)) {
        result.note = error;
        return result;
    }
    const std::vector<u8> bytes = serialize_cooked_mesh(mesh);

    std::error_code ec;
    const std::filesystem::path parent = std::filesystem::path(output_path).parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
    }
    std::ofstream out(output_path, std::ios::binary | std::ios::trunc);
    if (!out) {
        result.note = "unable to write cooked mesh output";
        return result;
    }
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    result.ok = out.good();
    result.byteCount = static_cast<u32>(bytes.size());
    result.note = result.ok ? ("mesh cooked, vertices=" + std::to_string(mesh.vertex_count()) +
                               " triangles=" + std::to_string(mesh.indices.size() / 3u))
                            : "cooked mesh write failed";
    return result;
}

bool load_cooked_mesh(const std::string& path, CookedMesh& out, std::string* error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        set_error(error, "cooked mesh unreadable");
        out = CookedMesh{};
        return false;
    }
    const std::vector<u8> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return deserialize_cooked_mesh(bytes.data(), bytes.size(), out, error);
}

} // namespace fuse::cook
