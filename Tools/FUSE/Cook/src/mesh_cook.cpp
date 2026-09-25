#include <fuse/cook/mesh_cook.hpp>

#include "mesh_cook_meshopt.hpp"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>

#include <cctype>
#include <cmath>
#include <cstdint>
#include <map>

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

void patch_u32(std::vector<u8>& out, usize at, u32 value) {
    for (u32 i = 0; i < 4u; ++i) {
        out[at + i] = static_cast<u8>((value >> (i * 8u)) & 0xFFu);
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
                         aiProcess_ValidateDataStructure;
    if (!options.import_skin) {
        flags |= aiProcess_PreTransformVertices; // deletes bones, so skinned imports keep bind-pose space
    }
    if (options.generate_normals) {
        flags |= aiProcess_GenSmoothNormals;
    }
    if (options.import_tangents) {
        flags |= aiProcess_CalcTangentSpace;
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

    // FMSH v2 streams are kept only when every imported sub-mesh provides them.
    bool allTangents = options.import_tangents;
    bool allUv1 = options.import_uv1;
    bool allColors = options.import_colors;
    bool anySkin = false;
    bool allSkin = options.import_skin;
    std::map<std::string, u16> jointIds; // bone name -> joint index (first-appearance order)
    std::vector<std::string> jointOrder;
    for (u32 meshIndex = 0; meshIndex < scene->mNumMeshes; ++meshIndex) {
        const aiMesh* mesh = scene->mMeshes[meshIndex];
        if (mesh == nullptr || (mesh->mPrimitiveTypes & aiPrimitiveType_TRIANGLE) == 0u || mesh->mNumVertices == 0u) {
            continue;
        }
        allTangents = allTangents && mesh->HasTangentsAndBitangents() && mesh->HasNormals();
        allUv1 = allUv1 && mesh->HasTextureCoords(1);
        allColors = allColors && mesh->HasVertexColors(0);
        anySkin = anySkin || (options.import_skin && mesh->HasBones());
        allSkin = allSkin && mesh->HasBones();
    }
    if (anySkin && !allSkin) {
        return reject(CookFailure::InvalidGeometry, "mesh import rejected: bones on some sub-meshes but not all");
    }

    for (u32 meshIndex = 0; meshIndex < scene->mNumMeshes; ++meshIndex) {
        const aiMesh* mesh = scene->mMeshes[meshIndex];
        if (mesh == nullptr || (mesh->mPrimitiveTypes & aiPrimitiveType_TRIANGLE) == 0u || mesh->mNumVertices == 0u) {
            continue;
        }
        if (allTangents) {
            for (u32 v = 0; v < mesh->mNumVertices; ++v) {
                const aiVector3D& t = mesh->mTangents[v];
                const aiVector3D& b = mesh->mBitangents[v];
                const aiVector3D& n = mesh->mNormals[v];
                const aiVector3D c = n ^ t; // cross(n, t)
                const f32 sign = (c * b) < 0.f ? -1.f : 1.f;
                out.tangents.insert(out.tangents.end(), {t.x, t.y, t.z, sign});
            }
        }
        if (allUv1) {
            for (u32 v = 0; v < mesh->mNumVertices; ++v) {
                const aiVector3D& t = mesh->mTextureCoords[1][v];
                out.uv1s.insert(out.uv1s.end(), {t.x, t.y});
            }
        }
        if (allColors) {
            for (u32 v = 0; v < mesh->mNumVertices; ++v) {
                const aiColor4D& c = mesh->mColors[0][v];
                for (f32 channel : {c.r, c.g, c.b, c.a}) {
                    if (!std::isfinite(channel)) {
                        return reject(CookFailure::InvalidGeometry, "mesh import rejected: non-finite vertex colour");
                    }
                    out.colors.push_back(static_cast<u8>(std::lround(std::clamp(channel, 0.f, 1.f) * 255.f)));
                }
            }
        }
        if (anySkin) {
            std::vector<std::vector<std::pair<f32, u16>>> influences(mesh->mNumVertices);
            for (u32 b = 0; b < mesh->mNumBones; ++b) {
                const aiBone* bone = mesh->mBones[b];
                const std::string name = bone->mName.C_Str();
                auto found = jointIds.find(name);
                if (found == jointIds.end()) {
                    if (jointOrder.size() >= 65535u) {
                        return reject(CookFailure::InvalidGeometry, "mesh import rejected: more than 65535 joints");
                    }
                    found = jointIds.emplace(name, static_cast<u16>(jointOrder.size())).first;
                    jointOrder.push_back(name);
                }
                for (u32 w = 0; w < bone->mNumWeights; ++w) {
                    const aiVertexWeight& weight = bone->mWeights[w];
                    if (weight.mVertexId >= mesh->mNumVertices || !std::isfinite(weight.mWeight) || weight.mWeight < 0.f) {
                        return reject(CookFailure::InvalidGeometry, "mesh import rejected: invalid skin weight on bone '" +
                                                                        name + "'");
                    }
                    if (weight.mWeight > 0.f) {
                        influences[weight.mVertexId].emplace_back(weight.mWeight, found->second);
                    }
                }
            }
            for (u32 v = 0; v < mesh->mNumVertices; ++v) {
                auto& list = influences[v];
                // Largest weights first; ties by joint index so the order never depends on bone order.
                std::sort(list.begin(), list.end(), [](const auto& a, const auto& b) {
                    return a.first != b.first ? a.first > b.first : a.second < b.second;
                });
                if (list.size() > 4u) {
                    list.resize(4u);
                }
                f32 sum = 0.f;
                for (const auto& entry : list) {
                    sum += entry.first;
                }
                if (list.empty() || sum <= 0.f) {
                    return reject(CookFailure::InvalidGeometry, "mesh import rejected: skinned vertex " +
                                                                    std::to_string(v) + " has no skin weights");
                }
                for (u32 k = 0; k < 4u; ++k) {
                    out.joints.push_back(k < list.size() ? list[k].second : static_cast<u16>(0));
                    out.weights.push_back(k < list.size() ? list[k].first / sum : 0.f);
                }
            }
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
            if (options.import_material_names) {
                std::string slot;
                if (mesh->mMaterialIndex < scene->mNumMaterials && scene->mMaterials[mesh->mMaterialIndex] != nullptr) {
                    slot = scene->mMaterials[mesh->mMaterialIndex]->GetName().C_Str();
                }
                out.material_slots.push_back(slot);
            }
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
    if (!all_finite(out.tangents) || !all_finite(out.uv1s)) {
        return reject(CookFailure::InvalidGeometry, "mesh import rejected: non-finite (NaN/Inf) tangent or uv1");
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

namespace {

std::vector<u8> serialize_v1(const CookedMesh& mesh) {
    const u32 vertexCount = mesh.vertex_count();
    std::vector<u8> out;
    out.reserve(kHeaderBytes + mesh.submeshes.size() * kSubmeshBytes + vertexCount * 32u +
                mesh.indices.size() * 4u + kTrailerBytes);
    out.insert(out.end(), std::begin(kMagic), std::end(kMagic));
    put_u32(out, kCookedMeshVersionV1);
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

void put_u16(std::vector<u8>& out, u32 value) {
    out.push_back(static_cast<u8>(value & 0xFFu));
    out.push_back(static_cast<u8>((value >> 8) & 0xFFu));
}

u32 get_u16(const u8* data) {
    return static_cast<u32>(data[0]) | (static_cast<u32>(data[1]) << 8);
}

void pad4(std::vector<u8>& out) {
    while (out.size() % 4u != 0u) {
        out.push_back(0u);
    }
}

s32 to_snorm16(f32 value) {
    return static_cast<s32>(std::lround(std::clamp(value, -1.f, 1.f) * 32767.f));
}

f32 from_snorm16(u32 bits) {
    const s32 v = static_cast<s32>(static_cast<std::int16_t>(static_cast<u16>(bits)));
    return std::max(static_cast<f32>(v) / 32767.f, -1.f);
}

void oct_encode(const f32* n, s32 out[2]) {
    const f32 l1 = std::fabs(n[0]) + std::fabs(n[1]) + std::fabs(n[2]);
    if (l1 <= 0.f) {
        out[0] = 0;
        out[1] = 0;
        return;
    }
    f32 x = n[0] / l1;
    f32 y = n[1] / l1;
    if (n[2] < 0.f) {
        const f32 ox = (1.f - std::fabs(y)) * (x >= 0.f ? 1.f : -1.f);
        const f32 oy = (1.f - std::fabs(x)) * (y >= 0.f ? 1.f : -1.f);
        x = ox;
        y = oy;
    }
    out[0] = to_snorm16(x);
    out[1] = to_snorm16(y);
}

void oct_decode(u32 bx, u32 by, f32* n) {
    f32 x = from_snorm16(bx);
    f32 y = from_snorm16(by);
    const f32 z = 1.f - std::fabs(x) - std::fabs(y);
    const f32 t = std::max(-z, 0.f);
    x += x >= 0.f ? -t : t;
    y += y >= 0.f ? -t : t;
    const f32 len = std::sqrt(x * x + y * y + z * z);
    n[0] = x / len;
    n[1] = y / len;
    n[2] = z / len;
}

/// Quantise 4 weights to `scale` (65535 or 255) so that they sum to exactly `scale`: round each, then
/// put the rounding residue on the largest weight (lowest slot on ties).
void quantize_weights(const f32* w, u32 scale, u32 out[4]) {
    s64 sum = 0;
    u32 largest = 0;
    for (u32 i = 0; i < 4u; ++i) {
        out[i] = static_cast<u32>(std::lround(std::clamp(w[i], 0.f, 1.f) * static_cast<f32>(scale)));
        sum += out[i];
        if (w[i] > w[largest]) {
            largest = i;
        }
    }
    const s64 fixed = static_cast<s64>(out[largest]) + (static_cast<s64>(scale) - sum);
    out[largest] = static_cast<u32>(std::clamp<s64>(fixed, 0, scale));
}

u32 stream_element_bytes(MeshStreamFormat format) {
    switch (format) {
    case MeshStreamFormat::F32x2:
        return 8u;
    case MeshStreamFormat::F32x3:
        return 12u;
    case MeshStreamFormat::Unorm16x3:
        return 6u;
    case MeshStreamFormat::OctSnorm16x2:
        return 4u;
    case MeshStreamFormat::OctSnorm16x2Sign:
        return 8u;
    case MeshStreamFormat::Unorm8x4:
    case MeshStreamFormat::Uint8x4:
        return 4u;
    case MeshStreamFormat::Uint16x4:
    case MeshStreamFormat::Unorm16x4:
        return 8u;
    }
    return 0u;
}

bool stream_format_allowed(MeshStreamSemantic semantic, MeshStreamFormat format) {
    switch (semantic) {
    case MeshStreamSemantic::Position:
        return format == MeshStreamFormat::F32x3 || format == MeshStreamFormat::Unorm16x3;
    case MeshStreamSemantic::Normal:
        return format == MeshStreamFormat::F32x3 || format == MeshStreamFormat::OctSnorm16x2;
    case MeshStreamSemantic::Uv0:
    case MeshStreamSemantic::Uv1:
        return format == MeshStreamFormat::F32x2;
    case MeshStreamSemantic::Tangent:
        return format == MeshStreamFormat::OctSnorm16x2Sign;
    case MeshStreamSemantic::Color0:
        return format == MeshStreamFormat::Unorm8x4;
    case MeshStreamSemantic::Joints0:
        return format == MeshStreamFormat::Uint8x4 || format == MeshStreamFormat::Uint16x4;
    case MeshStreamSemantic::Weights0:
        return format == MeshStreamFormat::Unorm8x4 || format == MeshStreamFormat::Unorm16x4;
    }
    return false;
}

bool use_meshopt_codec(const MeshEncoding& encoding) {
    return encoding.meshopt_codec && detail::meshopt_codec_available();
}

bool needs_v2(const CookedMesh& mesh, const MeshEncoding& encoding) {
    return encoding.quantize_positions || encoding.quantize_normals || !mesh.tangents.empty() ||
           !mesh.uv1s.empty() || !mesh.colors.empty() || !mesh.joints.empty() || !mesh.weights.empty() ||
           !mesh.material_slots.empty() || use_meshopt_codec(encoding) || detail::has_sections(mesh);
}

constexpr usize kHeaderBytesV2 = kHeaderBytes + 8u; // + streamCount + materialSlotCount
constexpr u32 kFlagQuantizedPositions = 1u;
constexpr u32 kFlagMeshoptCodec = 1u << 4; // W0.2 (bits 1..3 reserved)
constexpr u32 kFlagSections = 1u << 5;     // W0.2

} // namespace

std::vector<u8> serialize_cooked_mesh(const CookedMesh& mesh, const MeshEncoding& encoding) {
    if (!needs_v2(mesh, encoding)) {
        return serialize_v1(mesh);
    }
    const u32 vertexCount = mesh.vertex_count();
    const usize n = vertexCount;
    const bool codec = use_meshopt_codec(encoding);
    const bool sections = detail::has_sections(mesh);
    std::vector<u8> out;
    out.insert(out.end(), std::begin(kMagic), std::end(kMagic));
    put_u32(out, kCookedMeshVersion);
    put_u32(out, (encoding.quantize_positions ? kFlagQuantizedPositions : 0u) | (codec ? kFlagMeshoptCodec : 0u) |
                     (sections ? kFlagSections : 0u));
    put_u32(out, vertexCount);
    put_u32(out, static_cast<u32>(mesh.indices.size()));
    put_u32(out, static_cast<u32>(mesh.submeshes.size()));
    for (f32 value : mesh.bounds_min) {
        put_f32(out, value);
    }
    for (f32 value : mesh.bounds_max) {
        put_f32(out, value);
    }
    const usize streamCountAt = out.size();
    put_u32(out, 0u); // stream count (patched)
    put_u32(out, static_cast<u32>(mesh.material_slots.size()));
    for (const CookedMesh::Submesh& submesh : mesh.submeshes) {
        put_u32(out, submesh.index_offset);
        put_u32(out, submesh.index_count);
        put_u32(out, submesh.vertex_offset);
        put_u32(out, submesh.material_index);
    }
    for (const std::string& name : mesh.material_slots) {
        put_u32(out, static_cast<u32>(name.size()));
        out.insert(out.end(), name.begin(), name.end());
        pad4(out);
    }

    u32 streamCount = 0;
    // A stream is its table entry followed by its payload. With the meshopt codec the raw payload is
    // written first, then replaced by its encoding (and the entry's encoded size patched) when the
    // next stream begins or the table ends.
    usize openEntryAt = 0;
    usize openPayloadAt = 0;
    u32 openElementBytes = 0;
    auto end_stream = [&]() {
        if (openElementBytes == 0u) {
            return;
        }
        if (codec) {
            const std::vector<u8> raw(out.begin() + static_cast<std::ptrdiff_t>(openPayloadAt),
                                      out.begin() + static_cast<std::ptrdiff_t>(openPayloadAt + n * openElementBytes));
            out.resize(openPayloadAt);
            std::vector<u8> encoded;
            detail::meshopt_encode_vertices(raw.data(), n, openElementBytes, encoded);
            patch_u32(out, openEntryAt + 12u, static_cast<u32>(encoded.size()));
            out.insert(out.end(), encoded.begin(), encoded.end());
        }
        pad4(out);
        openElementBytes = 0;
    };
    auto begin_stream = [&](MeshStreamSemantic semantic, MeshStreamFormat format) {
        end_stream();
        openEntryAt = out.size();
        put_u32(out, static_cast<u32>(semantic));
        put_u32(out, static_cast<u32>(format));
        put_u32(out, static_cast<u32>(n * stream_element_bytes(format)));
        if (codec) {
            put_u32(out, 0u); // encoded bytes (patched by end_stream)
        }
        openPayloadAt = out.size();
        openElementBytes = stream_element_bytes(format);
        ++streamCount;
    };

    if (encoding.quantize_positions) {
        begin_stream(MeshStreamSemantic::Position, MeshStreamFormat::Unorm16x3);
        for (usize v = 0; v < n; ++v) {
            for (u32 axis = 0; axis < 3u; ++axis) {
                const f32 extent = mesh.bounds_max[axis] - mesh.bounds_min[axis];
                const f32 t = extent > 0.f ? (mesh.positions[v * 3u + axis] - mesh.bounds_min[axis]) / extent : 0.f;
                put_u16(out, static_cast<u32>(std::lround(std::clamp(t, 0.f, 1.f) * 65535.f)));
            }
        }
    } else {
        begin_stream(MeshStreamSemantic::Position, MeshStreamFormat::F32x3);
        for (usize v = 0; v < n * 3u; ++v) {
            put_f32(out, mesh.positions[v]);
        }
    }
    pad4(out);

    if (encoding.quantize_normals) {
        begin_stream(MeshStreamSemantic::Normal, MeshStreamFormat::OctSnorm16x2);
        for (usize v = 0; v < n; ++v) {
            const f32 zero[3] = {0.f, 0.f, 0.f};
            s32 oct[2];
            oct_encode(v * 3u + 2u < mesh.normals.size() ? &mesh.normals[v * 3u] : zero, oct);
            put_u16(out, static_cast<u32>(oct[0]) & 0xFFFFu);
            put_u16(out, static_cast<u32>(oct[1]) & 0xFFFFu);
        }
    } else {
        begin_stream(MeshStreamSemantic::Normal, MeshStreamFormat::F32x3);
        for (usize v = 0; v < n * 3u; ++v) {
            put_f32(out, v < mesh.normals.size() ? mesh.normals[v] : 0.f);
        }
    }
    pad4(out);

    begin_stream(MeshStreamSemantic::Uv0, MeshStreamFormat::F32x2);
    for (usize v = 0; v < n * 2u; ++v) {
        put_f32(out, v < mesh.uvs.size() ? mesh.uvs[v] : 0.f);
    }

    if (mesh.tangents.size() == n * 4u && n > 0u) {
        begin_stream(MeshStreamSemantic::Tangent, MeshStreamFormat::OctSnorm16x2Sign);
        for (usize v = 0; v < n; ++v) {
            s32 oct[2];
            oct_encode(&mesh.tangents[v * 4u], oct);
            put_u16(out, static_cast<u32>(oct[0]) & 0xFFFFu);
            put_u16(out, static_cast<u32>(oct[1]) & 0xFFFFu);
            put_u16(out, mesh.tangents[v * 4u + 3u] < 0.f ? 0xFFFFu : 1u);
            put_u16(out, 0u);
        }
    }
    if (mesh.uv1s.size() == n * 2u && n > 0u) {
        begin_stream(MeshStreamSemantic::Uv1, MeshStreamFormat::F32x2);
        for (usize v = 0; v < n * 2u; ++v) {
            put_f32(out, mesh.uv1s[v]);
        }
    }
    if (mesh.colors.size() == n * 4u && n > 0u) {
        begin_stream(MeshStreamSemantic::Color0, MeshStreamFormat::Unorm8x4);
        out.insert(out.end(), mesh.colors.begin(), mesh.colors.end());
    }
    if (mesh.joints.size() == n * 4u && mesh.weights.size() == n * 4u && n > 0u) {
        const bool wide = std::any_of(mesh.joints.begin(), mesh.joints.end(), [](u16 j) { return j > 255u; });
        begin_stream(MeshStreamSemantic::Joints0, wide ? MeshStreamFormat::Uint16x4 : MeshStreamFormat::Uint8x4);
        for (u16 joint : mesh.joints) {
            if (wide) {
                put_u16(out, joint);
            } else {
                out.push_back(static_cast<u8>(joint));
            }
        }
        const u32 scale = encoding.weights_unorm8 ? 255u : 65535u;
        begin_stream(MeshStreamSemantic::Weights0,
                     encoding.weights_unorm8 ? MeshStreamFormat::Unorm8x4 : MeshStreamFormat::Unorm16x4);
        for (usize v = 0; v < n; ++v) {
            u32 q[4];
            quantize_weights(&mesh.weights[v * 4u], scale, q);
            for (u32 value : q) {
                if (encoding.weights_unorm8) {
                    out.push_back(static_cast<u8>(value));
                } else {
                    put_u16(out, value);
                }
            }
        }
    }
    end_stream();
    patch_u32(out, streamCountAt, streamCount);

    if (codec) {
        std::vector<u8> encoded;
        detail::meshopt_encode_indices(mesh.indices.data(), mesh.indices.size(), vertexCount, encoded);
        put_u32(out, static_cast<u32>(encoded.size()));
        out.insert(out.end(), encoded.begin(), encoded.end());
        pad4(out);
    } else {
        for (u32 index : mesh.indices) {
            put_u32(out, index);
        }
    }
    if (sections) {
        detail::write_sections(mesh, codec, out);
    }
    put_u64(out, fnv1a64(out.data(), out.size()));
    return out;
}

namespace {

bool deserialize_v2(const u8* data, usize size, CookedMesh& out, std::string* error) {
    auto reject = [&](const std::string& message) {
        set_error(error, message);
        out = CookedMesh{};
        return false;
    };
    if (size < kHeaderBytesV2 + kTrailerBytes) {
        return reject("cooked mesh truncated");
    }
    if (get_u64(data + size - kTrailerBytes) != fnv1a64(data, size - kTrailerBytes)) {
        return reject("cooked mesh checksum mismatch");
    }
    const u32 flags = get_u32(data + 8);
    const u32 vertexCount = get_u32(data + 12);
    const u32 indexCount = get_u32(data + 16);
    const u32 submeshCount = get_u32(data + 20);
    const u32 streamCount = get_u32(data + 48);
    const u32 slotCount = get_u32(data + 52);
    if ((flags & ~(kFlagQuantizedPositions | kFlagMeshoptCodec | kFlagSections)) != 0u) {
        return reject("cooked mesh flags unsupported");
    }
    const bool codec = (flags & kFlagMeshoptCodec) != 0u;
    if (codec && !detail::meshopt_codec_available()) {
        return reject("cooked mesh uses the meshopt codec, which this build lacks");
    }
    if (slotCount != 0u && slotCount != submeshCount) {
        return reject("cooked mesh material slot count must be 0 or the submesh count");
    }
    const usize payloadEnd = size - kTrailerBytes;
    usize cursor = 24u;
    for (f32& value : out.bounds_min) {
        value = get_f32(data + cursor);
        cursor += 4u;
    }
    for (f32& value : out.bounds_max) {
        value = get_f32(data + cursor);
        cursor += 4u;
    }
    for (u32 axis = 0; axis < 3u; ++axis) {
        if (!std::isfinite(out.bounds_min[axis]) || !std::isfinite(out.bounds_max[axis]) ||
            out.bounds_min[axis] > out.bounds_max[axis]) {
            return reject("cooked mesh bounds invalid");
        }
    }
    cursor = kHeaderBytesV2;
    auto need = [&](u64 bytes) { return static_cast<u64>(cursor) + bytes <= payloadEnd; };
    if (!need(static_cast<u64>(submeshCount) * kSubmeshBytes)) {
        return reject("cooked mesh size mismatch");
    }
    out.submeshes.resize(submeshCount);
    for (CookedMesh::Submesh& submesh : out.submeshes) {
        submesh.index_offset = get_u32(data + cursor);
        submesh.index_count = get_u32(data + cursor + 4);
        submesh.vertex_offset = get_u32(data + cursor + 8);
        submesh.material_index = get_u32(data + cursor + 12);
        cursor += kSubmeshBytes;
        if (static_cast<u64>(submesh.index_offset) + submesh.index_count > indexCount ||
            submesh.vertex_offset > vertexCount) {
            return reject("cooked mesh submesh range out of bounds");
        }
    }
    for (u32 slot = 0; slot < slotCount; ++slot) {
        if (!need(4u)) {
            return reject("cooked mesh size mismatch");
        }
        const u32 length = get_u32(data + cursor);
        cursor += 4u;
        const u64 padded = (static_cast<u64>(length) + 3u) & ~static_cast<u64>(3u);
        if (!need(padded)) {
            return reject("cooked mesh material slot name out of bounds");
        }
        out.material_slots.emplace_back(reinterpret_cast<const char*>(data + cursor), length);
        cursor += static_cast<usize>(padded);
    }

    const usize n = vertexCount;
    u32 seen = 0;
    MeshStreamFormat weightFormat = MeshStreamFormat::Unorm16x4;
    std::vector<u32> rawWeights;
    std::vector<u8> decodedStream;
    for (u32 s = 0; s < streamCount; ++s) {
        const usize entryBytes = codec ? 16u : 12u;
        if (!need(entryBytes)) {
            return reject("cooked mesh stream table truncated");
        }
        const u32 semanticRaw = get_u32(data + cursor);
        const u32 formatRaw = get_u32(data + cursor + 4);
        const u32 byteLength = get_u32(data + cursor + 8);
        const u32 encodedBytes = codec ? get_u32(data + cursor + 12) : 0u;
        cursor += entryBytes;
        if (semanticRaw < 1u || semanticRaw > 8u) {
            return reject("cooked mesh stream semantic " + std::to_string(semanticRaw) + " unknown");
        }
        if ((seen & (1u << semanticRaw)) != 0u) {
            return reject("cooked mesh stream semantic " + std::to_string(semanticRaw) + " duplicated");
        }
        seen |= 1u << semanticRaw;
        const auto semantic = static_cast<MeshStreamSemantic>(semanticRaw);
        const auto format = static_cast<MeshStreamFormat>(formatRaw);
        if (formatRaw < 1u || formatRaw > 9u || !stream_format_allowed(semantic, format)) {
            return reject("cooked mesh stream " + std::to_string(semanticRaw) + " has unsupported format " +
                          std::to_string(formatRaw));
        }
        const u64 expected = static_cast<u64>(n) * stream_element_bytes(format);
        const u64 stored = codec ? static_cast<u64>(encodedBytes) : expected;
        const u64 padded = (stored + 3u) & ~static_cast<u64>(3u);
        if (byteLength != expected || !need(padded)) {
            return reject("cooked mesh stream " + std::to_string(semanticRaw) + " size mismatch");
        }
        const u8* p = data + cursor;
        if (codec) {
            if (!detail::meshopt_decode_vertices(p, encodedBytes, n, stream_element_bytes(format), decodedStream)) {
                return reject("cooked mesh stream " + std::to_string(semanticRaw) + " meshopt decode failed");
            }
            p = decodedStream.data();
        }
        switch (semantic) {
        case MeshStreamSemantic::Position:
            out.positions.resize(n * 3u);
            for (usize i = 0; i < n * 3u; ++i) {
                if (format == MeshStreamFormat::F32x3) {
                    out.positions[i] = get_f32(p + i * 4u);
                } else {
                    const u32 axis = static_cast<u32>(i % 3u);
                    const f32 t = static_cast<f32>(get_u16(p + i * 2u)) / 65535.f;
                    out.positions[i] = out.bounds_min[axis] + t * (out.bounds_max[axis] - out.bounds_min[axis]);
                }
            }
            break;
        case MeshStreamSemantic::Normal:
            out.normals.resize(n * 3u);
            for (usize v = 0; v < n; ++v) {
                if (format == MeshStreamFormat::F32x3) {
                    for (u32 d = 0; d < 3u; ++d) {
                        out.normals[v * 3u + d] = get_f32(p + (v * 3u + d) * 4u);
                    }
                } else {
                    oct_decode(get_u16(p + v * 4u), get_u16(p + v * 4u + 2u), &out.normals[v * 3u]);
                }
            }
            break;
        case MeshStreamSemantic::Uv0:
        case MeshStreamSemantic::Uv1: {
            std::vector<f32>& dst = semantic == MeshStreamSemantic::Uv0 ? out.uvs : out.uv1s;
            dst.resize(n * 2u);
            for (usize i = 0; i < n * 2u; ++i) {
                dst[i] = get_f32(p + i * 4u);
            }
            break;
        }
        case MeshStreamSemantic::Tangent:
            out.tangents.resize(n * 4u);
            for (usize v = 0; v < n; ++v) {
                const u32 sign = get_u16(p + v * 8u + 4u);
                if ((sign != 1u && sign != 0xFFFFu) || get_u16(p + v * 8u + 6u) != 0u) {
                    return reject("cooked mesh tangent sign invalid");
                }
                oct_decode(get_u16(p + v * 8u), get_u16(p + v * 8u + 2u), &out.tangents[v * 4u]);
                out.tangents[v * 4u + 3u] = sign == 1u ? 1.f : -1.f;
            }
            break;
        case MeshStreamSemantic::Color0:
            out.colors.assign(p, p + n * 4u);
            break;
        case MeshStreamSemantic::Joints0:
            out.joints.resize(n * 4u);
            for (usize i = 0; i < n * 4u; ++i) {
                out.joints[i] = static_cast<u16>(format == MeshStreamFormat::Uint8x4 ? p[i] : get_u16(p + i * 2u));
            }
            break;
        case MeshStreamSemantic::Weights0:
            weightFormat = format;
            rawWeights.resize(n * 4u);
            for (usize i = 0; i < n * 4u; ++i) {
                rawWeights[i] = format == MeshStreamFormat::Unorm8x4 ? p[i] : get_u16(p + i * 2u);
            }
            break;
        }
        cursor += static_cast<usize>(padded);
    }
    if ((seen & (1u << static_cast<u32>(MeshStreamSemantic::Position))) == 0u) {
        return reject("cooked mesh has no position stream");
    }
    const bool hasJoints = (seen & (1u << static_cast<u32>(MeshStreamSemantic::Joints0))) != 0u;
    const bool hasWeights = (seen & (1u << static_cast<u32>(MeshStreamSemantic::Weights0))) != 0u;
    if (hasJoints != hasWeights) {
        return reject("cooked mesh joints and weights must both be present or both absent");
    }
    if (hasWeights) {
        const u32 scale = weightFormat == MeshStreamFormat::Unorm8x4 ? 255u : 65535u;
        out.weights.resize(n * 4u);
        for (usize v = 0; v < n; ++v) {
            u32 sum = 0;
            for (u32 k = 0; k < 4u; ++k) {
                sum += rawWeights[v * 4u + k];
                out.weights[v * 4u + k] = static_cast<f32>(rawWeights[v * 4u + k]) / static_cast<f32>(scale);
            }
            if (sum != scale) {
                return reject("cooked mesh skin weights of vertex " + std::to_string(v) + " do not sum to 1");
            }
        }
    }
    if (out.normals.empty()) {
        out.normals.assign(n * 3u, 0.f);
    }
    if (out.uvs.empty()) {
        out.uvs.assign(n * 2u, 0.f);
    }
    if (codec) {
        if (!need(4u)) {
            return reject("cooked mesh size mismatch");
        }
        const u32 encodedBytes = get_u32(data + cursor);
        cursor += 4u;
        const u64 padded = (static_cast<u64>(encodedBytes) + 3u) & ~static_cast<u64>(3u);
        if (!need(padded) || indexCount % 3u != 0u) {
            return reject("cooked mesh size mismatch");
        }
        if (!detail::meshopt_decode_indices(data + cursor, encodedBytes, indexCount, out.indices)) {
            return reject("cooked mesh index meshopt decode failed");
        }
        cursor += static_cast<usize>(padded);
    } else {
        if (static_cast<u64>(cursor) + static_cast<u64>(indexCount) * 4u > payloadEnd) {
            return reject("cooked mesh size mismatch");
        }
        out.indices.resize(indexCount);
        for (u32& index : out.indices) {
            index = get_u32(data + cursor);
            cursor += 4u;
        }
    }
    for (u32 index : out.indices) {
        if (index >= vertexCount) {
            return reject("cooked mesh index out of range");
        }
    }
    if ((flags & kFlagSections) != 0u) {
        std::string why;
        usize consumed = 0;
        if (!detail::read_sections(data + cursor, payloadEnd - cursor, consumed, codec, out, &why)) {
            return reject("cooked mesh " + why);
        }
        cursor += consumed;
    }
    if (cursor != payloadEnd) {
        return reject("cooked mesh size mismatch");
    }
    for (f32 value : out.positions) {
        if (!std::isfinite(value)) {
            return reject("cooked mesh position not finite");
        }
    }
    return true;
}

} // namespace

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
    if (version == kCookedMeshVersion) {
        return deserialize_v2(data, size, out, error);
    }
    if (version != kCookedMeshVersionV1) {
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
    const MeshOptimizeOptions& optimize = options.optimize;
    if (optimize.lods && !build_mesh_lods(mesh, optimize.lod, &error)) {
        result.note = error;
        result.failure = mesh_optimizer_available() ? CookFailure::InvalidGeometry : CookFailure::ImporterUnavailable;
        return result;
    }
    if ((optimize.meshlets || optimize.cluster_dag) && !build_mesh_meshlets(mesh, optimize.cluster_dag, &error)) {
        result.note = error;
        result.failure = mesh_meshlets_available() ? CookFailure::InvalidGeometry : CookFailure::ImporterUnavailable;
        return result;
    }
    const std::vector<u8> bytes = serialize_cooked_mesh(mesh, options.encoding);

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
