#include <fuse/cook/mesh_cook.hpp>

#include "mesh_cook_meshopt.hpp"

#include <fuse/asset/detail/fmsh_layout.hpp>

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

// FMSH layout constants are shared with the runtime reader (fuse_asset, UNI-U7-ASSET-1).
constexpr const u8 (&kMagic)[4] = asset::detail::kFmshMagic;
constexpr usize kHeaderBytes = asset::detail::kFmshHeaderBytes;
constexpr usize kSubmeshBytes = asset::detail::kFmshSubmeshBytes;
constexpr usize kTrailerBytes = asset::detail::kFmshTrailerBytes;

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

u64 fnv1a64(const u8* data, usize size) { return asset::detail::fmsh_fnv1a64(data, size); }

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

void pad4(std::vector<u8>& out) {
    while (out.size() % 4u != 0u) {
        out.push_back(0u);
    }
}

s32 to_snorm16(f32 value) {
    return static_cast<s32>(std::lround(std::clamp(value, -1.f, 1.f) * 32767.f));
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

bool use_meshopt_codec(const MeshEncoding& encoding) {
    return encoding.meshopt_codec && detail::meshopt_codec_available();
}

bool needs_v2(const CookedMesh& mesh, const MeshEncoding& encoding) {
    return encoding.quantize_positions || encoding.quantize_normals || !mesh.tangents.empty() ||
           !mesh.uv1s.empty() || !mesh.colors.empty() || !mesh.joints.empty() || !mesh.weights.empty() ||
           !mesh.material_slots.empty() || use_meshopt_codec(encoding) || detail::has_sections(mesh);
}

using asset::detail::stream_element_bytes;
constexpr u32 kFlagQuantizedPositions = asset::detail::kFmshFlagQuantizedPositions;
constexpr u32 kFlagMeshoptCodec = asset::detail::kFmshFlagMeshoptCodec;
constexpr u32 kFlagSections = asset::detail::kFmshFlagSections;

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

// deserialize_cooked_mesh moved to the runtime asset library (Source/FUSE/Asset/src/cooked_mesh_reader.cpp).

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
    return asset::read_cooked_mesh_file(path, out, error);
}

} // namespace fuse::cook
