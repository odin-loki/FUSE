// FUSE Remaster W1.2: Torque DTS / DSQ -> POCO (see dts_poco.hpp).

#include <fuse/remaster/legacy_ts/dts_poco.hpp>

#include <fuse/animation/cooked_anim.hpp>
#include <fuse/cook/mesh_cook.hpp>
#include <fuse/remaster/digest.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <map>
#include <set>

namespace fuse::remaster::legacy_ts {

namespace fs = std::filesystem;

namespace {

bool fail(std::string* error, const std::string& message) {
    if (error != nullptr) {
        *error = message;
    }
    return false;
}

template <class T>
std::vector<u8> bytesOf(const std::vector<T>& v) {
    std::vector<u8> out(v.size() * sizeof(T));
    if (!v.empty()) {
        std::memcpy(out.data(), v.data(), out.size());
    }
    return out;
}

template <class T>
bool vectorOf(const std::vector<u8>& bytes, std::vector<T>& out) {
    if (bytes.size() % sizeof(T) != 0u) {
        return false;
    }
    out.resize(bytes.size() / sizeof(T));
    if (!bytes.empty()) {
        std::memcpy(out.data(), bytes.data(), bytes.size());
    }
    return true;
}

poco::Record header(poco::Kind kind, const std::string& id, const std::string& name, const ImportOptions& options,
                    const std::string& oaid, std::vector<std::string> tags) {
    poco::Record r;
    r.kind = kind;
    r.id = id;
    r.name = name;
    r.units.length = "m";
    r.units.metersPerUnit = 1.0;
    r.units.up = "+Y";
    r.units.handedness = "right";
    r.provenance.origin = options.origin;
    r.provenance.tool = options.tool;
    r.provenance.human_authorship =
        options.origin == poco::Origin::Authored ? poco::HumanAuthorship::Substantial : poco::HumanAuthorship::None;
    r.licence_id = options.licenceId;
    if (!oaid.empty()) {
        r.replaces.push_back("oaid:" + oaid);
    }
    r.tags = std::move(tags);
    r.payload = poco::defaultPayload(kind);
    return r;
}

void finishRecord(poco::Record& r) { r.distribution = poco::computeDistribution(r); }

std::optional<poco::BlobRef> putBlob(store::BlobStore& store, const std::vector<u8>& bytes, const std::string& media,
                                     std::string* error) {
    return store.put(std::span<const u8>(bytes.data(), bytes.size()), media, error);
}

struct Q {
    f64 x = 0, y = 0, z = 0, w = 1;
};
Q qmul(const Q& a, const Q& b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
void qrot(const Q& q, const f64 v[3], f64 out[3]) {
    // v' = v + 2w (u x v) + 2 u x (u x v)
    const f64 u[3] = {q.x, q.y, q.z};
    const f64 c1[3] = {u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]};
    const f64 c2[3] = {u[1] * c1[2] - u[2] * c1[1], u[2] * c1[0] - u[0] * c1[2], u[0] * c1[1] - u[1] * c1[0]};
    for (u32 i = 0; i < 3u; ++i) {
        out[i] = v[i] + 2.0 * q.w * c1[i] + 2.0 * c2[i];
    }
}

/// Column-major inverse default-pose world matrix of every joint, FUSE space.
std::vector<f32> inverseBindMatrices(const cook::DtsShape& shape, const std::vector<u32>& order) {
    const usize n = shape.nodes.size();
    std::vector<Q> worldQ(n);
    std::vector<std::array<f64, 3>> worldT(n);
    std::vector<u8> done(n, 0u);
    // Parents first: order guarantees it for joints whose parent chain is acyclic.
    for (u32 node : order) {
        const cook::DtsNode& d = shape.nodes[node];
        Q local{d.rotation[0], d.rotation[1], d.rotation[2], d.rotation[3]};
        const f64 len = std::sqrt(local.x * local.x + local.y * local.y + local.z * local.z + local.w * local.w);
        if (len > 0.0) {
            local = {local.x / len, local.y / len, local.z / len, local.w / len};
        }
        const f64 t[3] = {d.translation[0], d.translation[1], d.translation[2]};
        if (d.parent >= 0 && static_cast<usize>(d.parent) < n && done[static_cast<usize>(d.parent)] != 0u) {
            const usize p = static_cast<usize>(d.parent);
            f64 rt[3];
            qrot(worldQ[p], t, rt);
            worldQ[node] = qmul(worldQ[p], local);
            worldT[node] = {worldT[p][0] + rt[0], worldT[p][1] + rt[1], worldT[p][2] + rt[2]};
        } else {
            worldQ[node] = local;
            worldT[node] = {t[0], t[1], t[2]};
        }
        done[node] = 1u;
    }
    std::vector<f32> out;
    out.reserve(order.size() * 16u);
    for (u32 node : order) {
        // Torque -> FUSE: axis / translation (x, y, z) -> (x, z, -y).
        const Q& tq = worldQ[node];
        const Q q{tq.x, tq.z, -tq.y, tq.w};
        const f64 t[3] = {worldT[node][0], worldT[node][2], -worldT[node][1]};
        // Inverse: rotation conj(q), translation -(conj(q) t).
        const Q inv{-q.x, -q.y, -q.z, q.w};
        f64 it[3];
        qrot(inv, t, it);
        const f64 x = inv.x, y = inv.y, z = inv.z, w = inv.w;
        const f64 m[3][3] = {{1 - 2 * (y * y + z * z), 2 * (x * y - w * z), 2 * (x * z + w * y)},
                             {2 * (x * y + w * z), 1 - 2 * (x * x + z * z), 2 * (y * z - w * x)},
                             {2 * (x * z - w * y), 2 * (y * z + w * x), 1 - 2 * (x * x + y * y)}};
        for (u32 c = 0; c < 4u; ++c) {
            for (u32 r = 0; r < 4u; ++r) {
                f64 v = 0.0;
                if (c < 3u) {
                    v = r < 3u ? m[r][c] : 0.0;
                } else {
                    v = r < 3u ? -it[r] : 1.0;
                }
                out.push_back(static_cast<f32>(v));
            }
        }
    }
    return out;
}

std::string sanitize(const std::string& s) {
    std::string out;
    for (char c : s) {
        const unsigned char u = static_cast<unsigned char>(c);
        out.push_back(std::isalnum(u) != 0 || c == '_' || c == '-' ? static_cast<char>(std::tolower(u)) : '_');
    }
    return out.empty() ? std::string("unnamed") : out;
}

std::string mediaOfImage(const fs::path& p) {
    std::string ext = p.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (ext == ".png") return "image/png";
    if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
    if (ext == ".tga") return "image/x-tga";
    return "image/bmp";
}

/// A CookedMesh -> POCO Mesh payload (blobs stored). `jointOfNode` remaps skin joints (node -> skeleton joint).
bool meshPayload(store::BlobStore& store, const cook::CookedMesh& mesh, const std::vector<s32>& jointOfNode,
                 const std::string& materialPrefix, bool positionsOnly, poco::Mesh& out, std::string* error) {
    out = poco::Mesh{};
    auto stream = [&](poco::Semantic semantic, poco::Format format, const std::vector<u8>& bytes) {
        auto ref = putBlob(store, bytes, "fuse/mesh-stream", error);
        if (!ref) {
            return false;
        }
        out.streams.push_back(poco::MeshStream{semantic, format, *ref});
        return true;
    };
    if (!stream(poco::Semantic::Position, poco::Format::F32x3, bytesOf(mesh.positions))) {
        return false;
    }
    if (!positionsOnly) {
        if (!stream(poco::Semantic::Normal, poco::Format::F32x3, bytesOf(mesh.normals)) ||
            !stream(poco::Semantic::Uv0, poco::Format::F32x2, bytesOf(mesh.uvs))) {
            return false;
        }
        if (!mesh.weights.empty()) {
            std::vector<u16> joints = mesh.joints;
            for (u16& j : joints) {
                if (j >= jointOfNode.size() || jointOfNode[j] < 0) {
                    return fail(error, "skin joint " + std::to_string(j) + " is not a skeleton joint");
                }
                j = static_cast<u16>(jointOfNode[j]);
            }
            if (!stream(poco::Semantic::Joints0, poco::Format::U16x4, bytesOf(joints)) ||
                !stream(poco::Semantic::Weights0, poco::Format::F32x4, bytesOf(mesh.weights))) {
                return false;
            }
        }
    }
    auto indices = putBlob(store, bytesOf(mesh.indices), "fuse/mesh-stream", error);
    if (!indices) {
        return false;
    }
    out.indices32 = *indices;
    for (usize s = 0; s < mesh.submeshes.size(); ++s) {
        poco::Submesh sub;
        sub.indexOffset = mesh.submeshes[s].index_offset;
        sub.indexCount = mesh.submeshes[s].index_count;
        const std::string slot = s < mesh.material_slots.size() ? mesh.material_slots[s] : std::string();
        sub.materialSlot = slot.empty() || positionsOnly ? std::string() : materialPrefix + sanitize(slot);
        out.submeshes.push_back(sub);
    }
    for (u32 a = 0; a < 3u; ++a) {
        out.bounds[a] = mesh.bounds_min[a];
        out.bounds[3u + a] = mesh.bounds_max[a];
    }
    return true;
}

bool encodeClip(store::BlobStore& store, const animation::CookedAnim& anim, const std::string& skeletonId,
                poco::AnimClip& out, std::string* error) {
    std::string why;
    const std::vector<u8> bytes = animation::encode_cooked_anim(anim, &why);
    if (bytes.empty()) {
        return fail(error, "clip '" + anim.name + "': " + why);
    }
    auto ref = putBlob(store, bytes, "fuse/anim", error);
    if (!ref) {
        return false;
    }
    out = poco::AnimClip{};
    out.skeletonId = skeletonId;
    out.sampleRate = anim.sample_rate;
    out.duration = anim.duration;
    out.tracks = *ref;
    out.rootMotion = anim.has_root_motion;
    for (const animation::CookedAnimEvent& e : anim.events) {
        out.events.push_back(poco::AnimEvent{e.time, e.name});
    }
    return true;
}

std::optional<std::vector<u8>> readFile(const fs::path& p) { return store::readFileBytes(p); }

} // namespace

std::vector<const poco::Record*> ImportedShape::all() const {
    std::vector<const poco::Record*> out;
    for (const auto& r : lods) out.push_back(&r);
    for (const auto& r : collision) out.push_back(&r);
    if (skeleton) out.push_back(&*skeleton);
    for (const auto& r : clips) out.push_back(&r);
    for (const auto& r : textureSets) out.push_back(&r);
    for (const auto& r : materials) out.push_back(&r);
    return out;
}

bool importTorqueShape(const fs::path& dts, const std::vector<fs::path>& dsqs, store::BlobStore& store,
                       db::Database* database, const ImportOptions& options, ImportedShape& out, std::string* error) {
    out = ImportedShape{};
    const auto bytes = readFile(dts);
    if (!bytes || bytes->empty()) {
        return fail(error, "cannot read '" + dts.string() + "'");
    }
    cook::DtsShape shape;
    std::string why;
    if (!cook::parse_dts(bytes->data(), bytes->size(), shape, &why)) {
        return fail(error, dts.filename().string() + ": " + why);
    }
    const std::string fileSha = sha256Hex(bytes->data(), bytes->size());
    if (database != nullptr) {
        out.oaid = db::originalAssetId(options.gameId, "sha256", fileSha);
    }
    const std::string prefix =
        options.idPrefix.empty() ? sanitize(options.gameId) + "." + sanitize(dts.stem().string()) : options.idPrefix;
    const std::string stem = dts.stem().string();

    // Skeleton (every node, dts_joint_order).
    const std::vector<u32> order = cook::dts_joint_order(shape);
    std::vector<s32> jointOfNode(shape.nodes.size(), -1);
    for (u32 j = 0; j < order.size(); ++j) {
        jointOfNode[order[j]] = static_cast<s32>(j);
    }
    bool skinned = false;
    for (const cook::DtsMesh& m : shape.meshes) {
        skinned = skinned || m.type == cook::DtsMeshType::Skin;
    }
    const bool wantSkeleton = !shape.nodes.empty() && (skinned || !shape.sequences.empty() || !dsqs.empty());
    const std::string skeletonId = wantSkeleton ? prefix + ".skeleton" : std::string();
    if (wantSkeleton) {
        poco::Record r = header(poco::Kind::Skeleton, skeletonId, stem + " skeleton", options, out.oaid, {"torque", "dts"});
        poco::Skeleton& sk = std::get<poco::Skeleton>(r.payload);
        cook::dts_skeleton(shape, sk.joints, sk.parents);
        auto ref = putBlob(store, bytesOf(inverseBindMatrices(shape, order)), "fuse/matrix4x4-f32", error);
        if (!ref) {
            return false;
        }
        sk.inverseBind = *ref;
        finishRecord(r);
        out.skeleton = std::move(r);
    }

    // Materials (+ albedo textures found next to the shape).
    const std::string materialPrefix = prefix + ".material.";
    for (const std::string& name : shape.materials) {
        poco::Record mat = header(poco::Kind::Material, materialPrefix + sanitize(name), name, options, std::string(),
                                  {"torque", "dts"});
        for (const char* ext : {".png", ".jpg", ".jpeg", ".tga", ".bmp"}) {
            const fs::path image = dts.parent_path() / (name + ext);
            std::error_code ec;
            if (!fs::is_regular_file(image, ec)) {
                continue;
            }
            const auto imageBytes = readFile(image);
            if (!imageBytes) {
                return fail(error, "cannot read '" + image.string() + "'");
            }
            auto ref = putBlob(store, *imageBytes, mediaOfImage(image), error);
            if (!ref) {
                return false;
            }
            const std::string imageSha = sha256Hex(imageBytes->data(), imageBytes->size());
            poco::Record tex = header(poco::Kind::TextureSet, prefix + ".texture." + sanitize(name), name, options,
                                      database != nullptr ? db::originalAssetId(options.gameId, "sha256", imageSha) : "",
                                      {"torque", "texture"});
            poco::TextureSet& set = std::get<poco::TextureSet>(tex.payload);
            set.albedo = *ref;
            set.albedoSrgb = true;
            set.normalConvention = poco::NormalConvention::DX; // Torque normal maps are D3D-style (none here yet)
            finishRecord(tex);
            std::get<poco::Material>(mat.payload).textureSetId = tex.id;
            out.textureSets.push_back(std::move(tex));
            break;
        }
        finishRecord(mat);
        out.materials.push_back(std::move(mat));
    }

    // Visible details (largest size first) -> LOD meshes; negative sizes -> collision meshes.
    std::vector<u32> visible;
    std::vector<u32> hidden;
    for (u32 d = 0; d < shape.details.size(); ++d) {
        (shape.details[d].size >= 0.f ? visible : hidden).push_back(d);
    }
    std::stable_sort(visible.begin(), visible.end(),
                     [&](u32 a, u32 b) { return shape.details[a].size > shape.details[b].size; });
    cook::MeshCookOptions meshOptions;
    meshOptions.import_material_names = true;
    meshOptions.import_skin = true;
    meshOptions.generate_normals = true;
    for (usize l = 0; l < visible.size(); ++l) {
        cook::CookedMesh mesh;
        if (!cook::dts_detail_to_cooked_mesh(shape, visible[l], meshOptions, mesh, &why)) {
            return fail(error, stem + " detail '" + shape.details[visible[l]].name + "': " + why);
        }
        poco::Record r = header(poco::Kind::Mesh, prefix + ".mesh.lod" + std::to_string(l),
                                stem + " LOD" + std::to_string(l) + " (" + shape.details[visible[l]].name + ")", options,
                                l == 0u ? out.oaid : std::string(), {"torque", "dts", "lod" + std::to_string(l)});
        poco::Mesh& payload = std::get<poco::Mesh>(r.payload);
        if (!meshPayload(store, mesh, jointOfNode, materialPrefix, false, payload, error)) {
            return false;
        }
        if (!mesh.weights.empty()) {
            payload.skeletonId = skeletonId;
        }
        out.lods.push_back(std::move(r));
    }
    if (out.lods.empty()) {
        return fail(error, stem + ": no visible detail level");
    }
    for (usize l = 1; l < out.lods.size(); ++l) {
        std::get<poco::Mesh>(out.lods[0].payload).lodIds.push_back(out.lods[l].id);
    }
    for (poco::Record& r : out.lods) {
        finishRecord(r);
    }
    for (usize c = 0; c < hidden.size(); ++c) {
        cook::CookedMesh mesh;
        cook::MeshCookOptions plain;
        plain.generate_normals = false;
        if (!cook::dts_detail_to_cooked_mesh(shape, hidden[c], plain, mesh, &why)) {
            if (why.find("no meshes") != std::string::npos || why.find("no triangles") != std::string::npos) {
                continue; // an empty LOS / collision slot
            }
            return fail(error, stem + " detail '" + shape.details[hidden[c]].name + "': " + why);
        }
        poco::Record r = header(poco::Kind::Mesh, prefix + ".collision." + std::to_string(out.collision.size()),
                                stem + " " + shape.details[hidden[c]].name, options, std::string(),
                                {"torque", "dts", "collision"});
        if (!meshPayload(store, mesh, jointOfNode, materialPrefix, true, std::get<poco::Mesh>(r.payload), error)) {
            return false;
        }
        finishRecord(r);
        out.collision.push_back(std::move(r));
    }

    // Clips: the shape's sequences, then each dsq's (bound to the shape skeleton by name).
    cook::DtsAnimOptions animOptions;
    animOptions.sample_rate = options.sampleRate;
    animOptions.root_motion = options.rootMotion;
    std::set<std::string> clipIds;
    auto addClip = [&](const animation::CookedAnim& anim, const std::string& oaid, const std::string& source) {
        const std::string id = prefix + ".clip." + sanitize(anim.name);
        if (!clipIds.insert(id).second) {
            return fail(error, "duplicate sequence name '" + anim.name + "' (" + source + ")");
        }
        poco::Record r = header(poco::Kind::AnimClip, id, anim.name, options, oaid, {"torque", source});
        if (!encodeClip(store, anim, skeletonId, std::get<poco::AnimClip>(r.payload), error)) {
            return false;
        }
        finishRecord(r);
        out.clips.push_back(std::move(r));
        return true;
    };
    for (u32 s = 0; s < shape.sequences.size(); ++s) {
        animation::CookedAnim anim;
        if (!cook::dts_sequence_to_cooked_anim(shape, s, animOptions, anim, &why)) {
            return fail(error, stem + " sequence " + std::to_string(s) + ": " + why);
        }
        if (!addClip(anim, std::string(), "dts")) {
            return false;
        }
    }
    std::vector<std::pair<std::string, std::string>> dsqKeys; // (sha256, first clip id)
    for (const fs::path& path : dsqs) {
        const auto dsqBytes = readFile(path);
        cook::DsqFile dsq;
        if (!dsqBytes || !cook::parse_dsq(dsqBytes->data(), dsqBytes->size(), dsq, &why)) {
            return fail(error, path.filename().string() + ": " + (dsqBytes ? why : std::string("unreadable")));
        }
        const std::string sha = sha256Hex(dsqBytes->data(), dsqBytes->size());
        const std::string oaid = database != nullptr ? db::originalAssetId(options.gameId, "sha256", sha) : std::string();
        const usize first = out.clips.size();
        for (u32 s = 0; s < dsq.sequences.size(); ++s) {
            animation::CookedAnim anim;
            u32 unmatched = 0;
            if (!cook::dsq_sequence_to_cooked_anim(dsq, s, &shape, animOptions, anim, &why, &unmatched)) {
                return fail(error, path.filename().string() + " sequence " + std::to_string(s) + ": " + why);
            }
            if (unmatched != 0u) {
                return fail(error, path.filename().string() + " '" + anim.name + "': " + std::to_string(unmatched) +
                                       " animated node(s) not in the shape skeleton");
            }
            if (!addClip(anim, oaid, "dsq")) {
                return false;
            }
        }
        dsqKeys.emplace_back(sha, out.clips.size() > first ? out.clips[first].id : std::string());
    }

    for (const poco::Record* r : out.all()) {
        if (!store.writeRecord(*r, error)) {
            return false;
        }
    }

    if (database != nullptr) {
        auto oaid = database->addOriginalAsset(options.gameId, "mesh", "sha256", fileSha, "fuse_legacy_ts/dts", error);
        if (!oaid) {
            return false;
        }
        if (!database->setReplacement({*oaid, "default", "T0", "fuse_legacy_ts", out.lods[0].id, "mesh", json::Value()},
                                      error)) {
            return false;
        }
        for (const poco::Record& tex : out.textureSets) {
            const std::string texOaid = tex.replaces.empty() ? std::string() : tex.replaces[0].substr(5);
            const auto& albedo = std::get<poco::TextureSet>(tex.payload).albedo;
            if (texOaid.empty() || !albedo) {
                continue;
            }
            auto added = database->addOriginalAsset(options.gameId, "texture_set", "sha256", albedo->sha256,
                                                    "fuse_legacy_ts/texture", error);
            if (!added || !database->setReplacement({*added, "default", "T0", "fuse_legacy_ts", tex.id, "texture_set",
                                                     json::Value()},
                                                    error)) {
                return false;
            }
        }
        for (const auto& [sha, clipId] : dsqKeys) {
            auto added = database->addOriginalAsset(options.gameId, "anim_clip", "sha256", sha, "fuse_legacy_ts/dsq", error);
            if (!added) {
                return false;
            }
            out.dsqOaids.push_back(*added);
            if (!clipId.empty() &&
                !database->setReplacement({*added, "default", "T0", "fuse_legacy_ts", clipId, "anim_clip", json::Value()},
                                          error)) {
                return false;
            }
        }
    }
    return true;
}

bool loadPocoMesh(const store::BlobStore& store, const poco::Mesh& mesh, PocoMeshData& out, std::string* error) {
    out = PocoMeshData{};
    auto blob = [&](const poco::BlobRef& ref) -> std::optional<std::vector<u8>> {
        if (!store.verify(ref, error)) {
            return std::nullopt;
        }
        return store.get(ref.sha256);
    };
    for (const poco::MeshStream& s : mesh.streams) {
        const auto data = blob(s.data);
        if (!data) {
            return false;
        }
        bool ok = true;
        switch (s.semantic) {
        case poco::Semantic::Position: ok = s.format == poco::Format::F32x3 && vectorOf(*data, out.positions); break;
        case poco::Semantic::Normal: ok = s.format == poco::Format::F32x3 && vectorOf(*data, out.normals); break;
        case poco::Semantic::Uv0: ok = s.format == poco::Format::F32x2 && vectorOf(*data, out.uvs); break;
        case poco::Semantic::Joints0: ok = s.format == poco::Format::U16x4 && vectorOf(*data, out.joints); break;
        case poco::Semantic::Weights0: ok = s.format == poco::Format::F32x4 && vectorOf(*data, out.weights); break;
        default: break; // other attributes are not used by the cook route
        }
        if (!ok) {
            return fail(error, std::string("stream ") + poco::enumName(s.semantic) + ": unsupported format / size");
        }
    }
    const auto indices = blob(mesh.indices32);
    if (!indices || !vectorOf(*indices, out.indices)) {
        return fail(error, "indices32 unreadable");
    }
    const usize n = out.positions.size() / 3u;
    if (out.positions.size() % 3u != 0u || n == 0u || (!out.normals.empty() && out.normals.size() != n * 3u) ||
        (!out.uvs.empty() && out.uvs.size() != n * 2u) || out.joints.size() != out.weights.size() ||
        (!out.weights.empty() && out.weights.size() != n * 4u)) {
        return fail(error, "mesh streams disagree with the vertex count");
    }
    for (u32 i : out.indices) {
        if (i >= n) {
            return fail(error, "index out of range");
        }
    }
    return true;
}

} // namespace fuse::remaster::legacy_ts
