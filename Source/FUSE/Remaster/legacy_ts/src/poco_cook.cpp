// FUSE Remaster W1.3: POCO -> FMSH v2 / .fusetex / .fusemat / .fskl + assetcheck descriptor (see poco_cook.hpp).

#include <fuse/remaster/legacy_ts/poco_cook.hpp>

#include <fuse/animation/skeleton.hpp>
#include <fuse/animation/cooked_anim.hpp>
#include <fuse/assetcheck/asset_rules.hpp>
#include <fuse/asset/cooked_material.hpp>
#include <fuse/cook/dts_write.hpp>
#include <fuse/project/asset_cooker.hpp>
#include <fuse/project/import_desc.hpp>
#include <fuse/project/t3d_material_parse.hpp>
#include <fuse/remaster/legacy_ts/dts_poco.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace fuse::remaster::legacy_ts {

namespace fs = std::filesystem;

namespace {

bool fail(std::string* error, const std::string& message) {
    if (error != nullptr) {
        *error = message;
    }
    return false;
}

std::string fmtFloat(f32 v) {
    char b[48];
    std::snprintf(b, sizeof(b), "%.6g", static_cast<double>(v));
    return b;
}

std::string jsonString(const std::string& s) {
    std::string out = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') {
            out.push_back('\\');
        }
        out.push_back(c);
    }
    return out + "\"";
}

std::string extOfMedia(const std::string& media) {
    if (media == "image/png") return ".png";
    if (media == "image/jpeg") return ".jpg";
    if (media == "image/x-tga") return ".tga";
    return ".bmp";
}

/// Invert a column-major rigid 4x4 (rotation transposed, translation -R^T t).
fuse::animation::mat4 rigidInverse(const f32* m) {
    fuse::animation::mat4 out = fuse::animation::mat4::identity();
    for (u32 r = 0; r < 3u; ++r) {
        for (u32 c = 0; c < 3u; ++c) {
            out.data[c * 4u + r] = m[r * 4u + c];
        }
    }
    for (u32 r = 0; r < 3u; ++r) {
        f32 v = 0.f;
        for (u32 k = 0; k < 3u; ++k) {
            v += out.data[k * 4u + r] * m[12u + k];
        }
        out.data[12u + r] = -v;
    }
    return out;
}

} // namespace

const poco::Record* PocoRecordSet::find(const std::string& id) const {
    for (const poco::Record& r : records) {
        if (r.id == id) {
            return &r;
        }
    }
    return nullptr;
}

bool pocoMeshToCooked(const store::BlobStore& store, const poco::Mesh& mesh, const PocoRecordSet& records,
                      cook::CookedMesh& out, std::string* error) {
    out = cook::CookedMesh{};
    PocoMeshData data;
    if (!loadPocoMesh(store, mesh, data, error)) {
        return false;
    }
    const usize n = data.positions.size() / 3u;
    out.positions = std::move(data.positions);
    out.normals = data.normals.empty() ? std::vector<f32>(n * 3u, 0.f) : std::move(data.normals);
    out.uvs = data.uvs.empty() ? std::vector<f32>(n * 2u, 0.f) : std::move(data.uvs);
    out.indices = std::move(data.indices);
    out.joints = std::move(data.joints);
    out.weights = std::move(data.weights);
    for (const poco::Submesh& sub : mesh.submeshes) {
        if (static_cast<u64>(sub.indexOffset) + sub.indexCount > out.indices.size()) {
            return fail(error, "submesh outside the index list");
        }
        cook::CookedMesh::Submesh s;
        s.index_offset = sub.indexOffset;
        s.index_count = sub.indexCount;
        s.material_index = static_cast<u32>(out.submeshes.size());
        out.submeshes.push_back(s);
        const poco::Record* material = sub.materialSlot.empty() ? nullptr : records.find(sub.materialSlot);
        out.material_slots.push_back(material != nullptr ? material->name : sub.materialSlot);
    }
    if (out.submeshes.empty()) {
        out.submeshes.push_back(cook::CookedMesh::Submesh{0u, static_cast<u32>(out.indices.size()), 0u, 0u});
        out.material_slots.push_back(std::string());
    }
    for (u32 a = 0; a < 3u; ++a) {
        out.bounds_min[a] = mesh.bounds[a];
        out.bounds_max[a] = mesh.bounds[3u + a];
    }
    return true;
}

bool cookPocoMesh(const store::BlobStore& store, const poco::Mesh& mesh, const PocoRecordSet& records,
                  const fs::path& outPath, std::string* error, std::vector<u8>* bytesOut) {
    cook::CookedMesh cooked;
    if (!pocoMeshToCooked(store, mesh, records, cooked, error)) {
        return false;
    }
    const std::vector<u8> bytes = cook::serialize_cooked_mesh(cooked);
    // The strict cook's §5.3 mesh gates, on the bytes as they will be read.
    cook::CookedMesh loaded;
    std::string why;
    if (!cook::deserialize_cooked_mesh(bytes.data(), bytes.size(), loaded, &why)) {
        return fail(error, "FMSH does not read back: " + why);
    }
    assetcheck::Violations v;
    assetcheck::checkTriangles(loaded.positions.data(), loaded.vertex_count(), loaded.indices.data(), loaded.indices.size(),
                               "mesh LOD0", v);
    if (!loaded.weights.empty()) {
        assetcheck::checkSkinWeights(loaded.joints.data(), loaded.weights.data(), loaded.vertex_count(), 4u, 0u, "mesh skin", v);
    }
    if (!v.empty()) {
        return fail(error, "asset gate failed (FUSE_ASSET_PLAN 5.3): " + v.front());
    }
    if (!cook::write_binary_file(outPath.string(), bytes, error)) {
        return false;
    }
    if (bytesOut != nullptr) {
        *bytesOut = bytes;
    }
    return true;
}

bool cookPocoShape(const store::BlobStore& store, const PocoRecordSet& records, const std::string& lod0Id,
                   const fs::path& dir, const PocoShapeCookOptions& options, PocoShapeCookResult& out, std::string* error) {
    out = PocoShapeCookResult{};
    const poco::Record* lod0 = records.find(lod0Id);
    if (lod0 == nullptr || lod0->kind != poco::Kind::Mesh) {
        return fail(error, "no mesh record '" + lod0Id + "'");
    }
    const std::string base = options.base;
    if (base.empty()) {
        return fail(error, "options.base is required (<name>_<variant>)");
    }
    std::error_code ec;
    fs::create_directories(dir, ec);
    const poco::Mesh& mesh0 = std::get<poco::Mesh>(lod0->payload);
    std::vector<const poco::Mesh*> lods = {&mesh0};
    for (const std::string& id : mesh0.lodIds) {
        const poco::Record* r = records.find(id);
        if (r == nullptr || r->kind != poco::Kind::Mesh) {
            return fail(error, "LOD record '" + id + "' missing");
        }
        lods.push_back(&std::get<poco::Mesh>(r->payload));
    }
    for (usize l = 0; l < lods.size(); ++l) {
        const fs::path path = dir / (base + "_lod" + std::to_string(l) + ".fusemesh");
        if (!cookPocoMesh(store, *lods[l], records, path, error)) {
            return false;
        }
        out.meshes.push_back(path.filename().string());
    }

    // Material + texture (the first submesh's slot).
    const poco::Record* material = nullptr;
    for (const poco::Submesh& sub : mesh0.submeshes) {
        if (!sub.materialSlot.empty() && (material = records.find(sub.materialSlot)) != nullptr) {
            break;
        }
    }
    std::string albedoCookId;
    if (material != nullptr) {
        const poco::Material& m = std::get<poco::Material>(material->payload);
        const poco::Record* tex = m.textureSetId.empty() ? nullptr : records.find(m.textureSetId);
        if (tex != nullptr && std::get<poco::TextureSet>(tex->payload).albedo) {
            const poco::BlobRef& ref = *std::get<poco::TextureSet>(tex->payload).albedo;
            const auto image = store.get(ref.sha256);
            if (!image || !store.verify(ref, error)) {
                return fail(error, "albedo blob " + ref.sha256 + " missing");
            }
            const fs::path source = dir / (base + "_albedo" + extOfMedia(ref.media));
            if (!cook::write_binary_file(source.string(), *image, error)) {
                return false;
            }
            const fs::path cooked = dir / (base + "_albedo.fusetex");
            project::TextureImportDesc desc;
            desc.input_path = source.string();
            desc.output_path = cooked.string();
            desc.generate_mipmaps = true;
            desc.color_space = std::get<poco::TextureSet>(tex->payload).albedoSrgb ? project::TextureImportDesc::ColorSpace::sRGB
                                                                                   : project::TextureImportDesc::ColorSpace::Linear;
            desc.compression = project::TextureImportDesc::Compression::BC7;
            project::AssetCooker cooker; // strict: the §5.3 texture gate applies
            const project::CookRecord record = cooker.cook_texture(desc);
            if (!record.ok) {
                return fail(error, "texture cook refused: " + record.note);
            }
            out.textureSource = source.filename().string();
            out.texture = cooked.filename().string();
            albedoCookId = cooked.stem().string();
        }
        asset::CookedMaterial cm;
        cm.name = material->name.substr(0, 255);
        cm.albedo[0] = std::clamp(m.baseColor[0], 0.02f, 0.9f); // the .fusemat validator's physical albedo range
        cm.albedo[1] = std::clamp(m.baseColor[1], 0.02f, 0.9f); // the .fusemat validator's physical albedo range
        cm.albedo[2] = std::clamp(m.baseColor[2], 0.02f, 0.9f); // the .fusemat validator's physical albedo range
        cm.roughness = std::clamp(m.roughness, 0.f, 1.f);
        cm.metallic = std::clamp(m.metallic, 0.f, 1.f);
        cm.textures.albedo = albedoCookId;
        const fs::path matPath = dir / (base + ".fusemat");
        if (!project::writeCookedMaterialFile(cm, matPath.string(), error)) {
            return false;
        }
        out.material = matPath.filename().string();
    }

    // Skeleton rig (skinned meshes).
    const poco::Record* skeleton = mesh0.skeletonId.empty() ? nullptr : records.find(mesh0.skeletonId);
    if (!mesh0.skeletonId.empty() && skeleton == nullptr) {
        return fail(error, "skeleton record '" + mesh0.skeletonId + "' missing");
    }
    if (skeleton != nullptr) {
        const poco::Skeleton& sk = std::get<poco::Skeleton>(skeleton->payload);
        std::vector<f32> inverseBind;
        if (sk.inverseBind) {
            const auto blob = store.get(sk.inverseBind->sha256);
            if (!blob || blob->size() != sk.joints.size() * 64u) {
                return fail(error, "inverse bind blob missing or mis-sized");
            }
            inverseBind.resize(blob->size() / 4u);
            std::memcpy(inverseBind.data(), blob->data(), blob->size());
        }
        fuse::animation::Skeleton rig;
        rig.bones.resize(sk.joints.size());
        for (usize j = 0; j < sk.joints.size(); ++j) {
            fuse::animation::Bone& b = rig.bones[j];
            std::strncpy(b.name, sk.joints[j].c_str(), sizeof(b.name) - 1u);
            b.parent_index = j < sk.parents.size() ? sk.parents[j] : -1;
            if (!inverseBind.empty()) {
                std::memcpy(b.inverse_bind.data.data(), &inverseBind[j * 16u], 64u);
                const fuse::animation::mat4 world = rigidInverse(&inverseBind[j * 16u]);
                // local bind = inverse(parent world) * world
                if (b.parent_index >= 0) {
                    const f32* pinv = &inverseBind[static_cast<usize>(b.parent_index) * 16u];
                    fuse::animation::mat4 local{};
                    for (u32 c = 0; c < 4u; ++c) {
                        for (u32 r = 0; r < 4u; ++r) {
                            f32 v = 0.f;
                            for (u32 k = 0; k < 4u; ++k) {
                                v += pinv[k * 4u + r] * world.data[c * 4u + k];
                            }
                            local.data[c * 4u + r] = v;
                        }
                    }
                    b.local_transform = local;
                } else {
                    b.local_transform = world;
                }
            }
        }
        rig.bone_count = static_cast<u32>(rig.bones.size());
        const fs::path rigPath = dir / (base + "_rig.fskl");
        if (!rig.save(rigPath.string().c_str())) {
            return fail(error, "cannot write " + rigPath.string());
        }
        out.rig = rigPath.filename().string();
        out.skeletonHash = fuse::animation::skeleton_hash_of_joints(sk.joints, sk.parents);
    }

    // fuse_assetcheck descriptor.
    out.assetId = options.assetClass + "/" + options.style + "/" + base;
    std::string d = "{\n  \"schema\": 1,\n  \"id\": " + jsonString(out.assetId) + ",\n  \"class\": " +
                    jsonString(options.assetClass) + ",\n  \"category\": " + jsonString(options.category) +
                    ",\n  \"units\": \"m\",\n  \"up\": \"y\",\n  \"tiling\": false,\n  \"dag\": false,\n  \"impostor\": "
                    "false,\n  \"hero\": false,\n  \"max_view_distance\": " +
                    fmtFloat(options.maxViewDistance) + ",\n  \"material\": {\"metal\": \"none\", \"emissive\": false}";
    d += ",\n  \"textures\": [";
    if (!out.textureSource.empty()) {
        d += "{\"map\": \"albedo\", \"path\": " + jsonString(out.textureSource) +
             ", \"colour_space\": \"srgb\", \"format\": \"BC7\"}";
    }
    d += "],\n  \"lods\": [";
    for (usize l = 0; l < out.meshes.size(); ++l) {
        const f32 err = l < options.lodErrors.size() ? options.lodErrors[l] : 0.01f * static_cast<f32>(l);
        d += std::string(l == 0u ? "" : ", ") + "{\"path\": " + jsonString(out.meshes[l]) + ", \"error\": " + fmtFloat(err) + "}";
    }
    d += "]";
    if (!out.rig.empty()) {
        char hash[24];
        std::snprintf(hash, sizeof(hash), "0x%016llx", static_cast<unsigned long long>(out.skeletonHash));
        d += ",\n  \"skin\": {\"rig\": " + jsonString(out.rig) + ", \"skeleton_hash\": \"" + hash + "\"}";
    }
    d += "\n}\n";
    const fs::path descriptor = dir / (base + ".asset.json");
    if (!store::writeFileAtomic(descriptor, std::string_view(d), error)) {
        return false;
    }
    out.descriptor = descriptor.filename().string();
    return true;
}

} // namespace fuse::remaster::legacy_ts
