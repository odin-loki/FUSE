#include <fuse/cook/mesh_cook.hpp>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>

#include <cctype>
#include <cmath>

#if defined(FUSE_HAS_ASSIMP)
#include <assimp/DefaultLogger.hpp>
#include <assimp/Importer.hpp>
#include <assimp/LogStream.hpp>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

#include <mutex>
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

void set_failure(CookFailure* failure, CookFailure value) {
    if (failure != nullptr) {
        *failure = value;
    }
}

std::string to_lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

/// Importer diagnostics that mean "the file parsed, but its geometry references data that does not
/// exist" (e.g. OBJ faces pointing past the vertex list, glTF indices past the accessor range).
[[maybe_unused]] bool mentions_out_of_range_geometry(const std::string& message) {
    const std::string lower = to_lower(message);
    return lower.find("out of range") != std::string::npos || lower.find("out-of-range") != std::string::npos ||
           lower.find("bad vertex index") != std::string::npos ||
           lower.find("invalid face index") != std::string::npos;
}

[[maybe_unused]] bool all_finite(const std::vector<f32>& values) {
    for (f32 value : values) {
        if (!std::isfinite(value)) {
            return false;
        }
    }
    return true;
}

[[maybe_unused]] std::string strip_log_prefix(std::string message) {
    // Assimp log lines look like "Warn,  T0: <message>\n".
    const std::size_t colon = message.find(": ");
    if (message.rfind("Warn", 0) == 0 && colon != std::string::npos) {
        message.erase(0, colon + 2);
    }
    while (!message.empty() && (message.back() == '\n' || message.back() == '\r')) {
        message.pop_back();
    }
    return message;
}

bool has_extension(const std::string& path, const char* ext) {
    const std::string lower = to_lower(std::filesystem::path(path).extension().string());
    return lower == ext;
}

/// OBJ is line-oriented text, so a file cut short usually still parses: assimp quietly accepts a
/// final `v 1.0 2` (missing z) or a dangling `f 1 2`. Strict import rejects statements whose
/// argument count is impossible, which is what truncation mid-line produces. (A cut exactly at a
/// line boundary is indistinguishable from a shorter valid file; faces referencing the lost
/// vertices are then caught as out-of-range indices.)
[[maybe_unused]] bool lint_obj_source(const std::string& input_path, std::string* error) {
    if (!has_extension(input_path, ".obj")) {
        return true;
    }
    std::ifstream in(input_path, std::ios::binary);
    if (!in) {
        return true; // the importer reports unreadable files itself
    }
    std::string line;
    u32 lineNumber = 0;
    while (std::getline(in, line)) {
        ++lineNumber;
        const std::size_t hash = line.find('#');
        if (hash != std::string::npos) {
            line.resize(hash);
        }
        std::istringstream tokens(line);
        std::string keyword;
        if (!(tokens >> keyword)) {
            continue;
        }
        u32 args = 0;
        for (std::string token; tokens >> token;) {
            ++args;
        }
        u32 required = 0;
        if (keyword == "v" || keyword == "vn") {
            required = 3u;
        } else if (keyword == "vt") {
            required = 1u;
        } else if (keyword == "f") {
            required = 3u;
        } else {
            continue;
        }
        if (args < required) {
            set_error(error, "mesh import failed: OBJ line " + std::to_string(lineNumber) + ": '" + keyword +
                                 "' has " + std::to_string(args) + " of " + std::to_string(required) +
                                 " required values (truncated file?)");
            return false;
        }
    }
    return true;
}

#if defined(FUSE_HAS_ASSIMP)
// Assimp reports some data problems only as log warnings while still returning a scene — e.g. the
// glTF2 importer drops faces with out-of-range indices and carries on. Strict import must see those,
// so a capture stream is attached to assimp's (global) logger once, and each import collects the
// warnings raised on its own thread.
thread_local std::vector<std::string>* t_importWarnings = nullptr;

class ImportWarningCapture final : public Assimp::LogStream {
public:
    void write(const char* message) override {
        if (t_importWarnings != nullptr && message != nullptr) {
            t_importWarnings->emplace_back(message);
        }
    }
};

void ensure_import_warning_capture() {
    static std::mutex mutex;
    static const Assimp::Logger* attachedTo = nullptr;
    std::lock_guard<std::mutex> lock(mutex);
    if (Assimp::DefaultLogger::isNullLogger()) {
        Assimp::DefaultLogger::create(nullptr, Assimp::Logger::NORMAL, 0u);
    }
    Assimp::Logger* logger = Assimp::DefaultLogger::get();
    if (logger != attachedTo) {
        // The logger owns (and deletes) attached streams.
        logger->attachStream(new ImportWarningCapture(), Assimp::Logger::Warn | Assimp::Logger::Err);
        attachedTo = logger;
    }
}

struct ScopedWarningCapture {
    std::vector<std::string> warnings;
    ScopedWarningCapture() {
        ensure_import_warning_capture();
        t_importWarnings = &warnings;
    }
    ~ScopedWarningCapture() { t_importWarnings = nullptr; }
    ScopedWarningCapture(const ScopedWarningCapture&) = delete;
    ScopedWarningCapture& operator=(const ScopedWarningCapture&) = delete;
};
#endif

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
                      std::string* error, CookFailure* failure) {
    out = CookedMesh{};
    set_failure(failure, CookFailure::None);
#if defined(FUSE_HAS_ASSIMP)
    auto reject = [&](CookFailure kind, const std::string& message) {
        set_error(error, message);
        set_failure(failure, kind);
        out = CookedMesh{};
        return false;
    };
    if (input_path.empty()) {
        return reject(CookFailure::InvalidArgument, "missing input path");
    }
    std::error_code ec;
    if (!std::filesystem::is_regular_file(std::filesystem::path(input_path), ec)) {
        return reject(CookFailure::InvalidArgument, "source file not found");
    }

    // Single-threaded, fixed post-process set: identical bytes in → identical mesh out.
    unsigned int flags = aiProcess_Triangulate | aiProcess_JoinIdenticalVertices | aiProcess_SortByPType |
                         aiProcess_PreTransformVertices | aiProcess_ValidateDataStructure;
    if (options.generate_normals) {
        flags |= aiProcess_GenSmoothNormals;
    }

    if (!lint_obj_source(input_path, error)) {
        set_failure(failure, CookFailure::MalformedSource);
        return false;
    }

    ScopedWarningCapture capture;
    Assimp::Importer importer;
    const aiScene* scene = importer.ReadFile(input_path, flags);
    std::string droppedGeometry;
    for (const std::string& warning : capture.warnings) {
        if (mentions_out_of_range_geometry(warning)) {
            droppedGeometry = strip_log_prefix(warning);
            break;
        }
    }
    if (scene == nullptr) {
        const std::string reason = importer.GetErrorString();
        if (!droppedGeometry.empty()) {
            return reject(CookFailure::InvalidGeometry,
                          "mesh import failed: " + reason + " (" + droppedGeometry + ")");
        }
        return reject(mentions_out_of_range_geometry(reason) ? CookFailure::InvalidGeometry
                                                             : CookFailure::MalformedSource,
                      "mesh import failed: " + reason);
    }
    if ((scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE) != 0u) {
        return reject(CookFailure::MalformedSource, "mesh import failed: scene is incomplete");
    }
    if (!droppedGeometry.empty()) {
        return reject(CookFailure::InvalidGeometry,
                      "mesh import rejected: importer dropped geometry (" + droppedGeometry + ")");
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
                if (face.mIndices[corner] >= mesh->mNumVertices) {
                    return reject(CookFailure::InvalidGeometry,
                                  "mesh import rejected: face " + std::to_string(f) + " index " +
                                      std::to_string(face.mIndices[corner]) + " out of range (vertices=" +
                                      std::to_string(mesh->mNumVertices) + ")");
                }
                out.indices.push_back(submesh.vertex_offset + face.mIndices[corner]);
            }
        }
        submesh.index_count = static_cast<u32>(out.indices.size()) - submesh.index_offset;
        if (submesh.index_count > 0u) {
            out.submeshes.push_back(submesh);
        }
    }

    if (out.indices.empty()) {
        return reject(CookFailure::InvalidGeometry, "mesh import produced no triangles");
    }
    if (!all_finite(out.positions)) {
        return reject(CookFailure::InvalidGeometry, "mesh import rejected: non-finite (NaN/Inf) vertex position");
    }
    if (!all_finite(out.normals)) {
        return reject(CookFailure::InvalidGeometry, "mesh import rejected: non-finite (NaN/Inf) vertex normal");
    }
    if (!all_finite(out.uvs)) {
        return reject(CookFailure::InvalidGeometry, "mesh import rejected: non-finite (NaN/Inf) texture coordinate");
    }
    compute_bounds(out);
    return true;
#else
    (void)input_path;
    (void)options;
    set_error(error, "assimp unavailable (library not linked)");
    set_failure(failure, CookFailure::ImporterUnavailable);
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
        result.failure = CookFailure::InvalidArgument;
        return result;
    }
    CookedMesh mesh;
    std::string error;
    CookFailure failure = CookFailure::None;
    if (!import_mesh_file(input_path, options, mesh, &error, &failure)) {
        result.note = error;
        result.failure = failure;
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
        result.failure = CookFailure::WriteFailed;
        return result;
    }
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    result.ok = out.good();
    result.failure = result.ok ? CookFailure::None : CookFailure::WriteFailed;
    result.byteCount = static_cast<u32>(bytes.size());
    result.note = result.ok ? ("mesh cooked, vertices=" + std::to_string(mesh.vertex_count()) +
                               " triangles=" + std::to_string(mesh.indices.size() / 3u))
                            : "cooked mesh write failed";
    if (result.ok && options.post_hook != nullptr) {
        out.close();
        std::string hookNote;
        if (!options.post_hook(mesh, bytes, output_path, &hookNote)) {
            result.ok = false;
            result.failure = CookFailure::WriteFailed;
            result.note = "mesh post-cook hook failed: " + hookNote;
        } else if (!hookNote.empty()) {
            result.note += "; " + hookNote;
        }
    }
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
