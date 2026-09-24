// FUSE Relight RL-1.8: the POCO + hash_key store shim (see poco_store.hpp).
#include <fuse/relight/capture/export/poco_store.hpp>

#include <fuse/relight/capture/export/dds.hpp>
#include <fuse/relight/capture/export/digest.hpp>
#include <fuse/relight/capture/export/json.hpp>
#include <fuse/relight/hash/hash_string.hpp>
#include <fuse/relight/hash/texture_hash.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <set>
#include <tuple>

namespace fuse::relight::capture::exporter {

namespace fs = std::filesystem;
using json::Value;

namespace {

const Uuid& oaidNamespace() {
    static const Uuid ns = uuidV5(uuidNamespaceUrl(), "https://fuse.invalid/remaster/oaid");
    return ns;
}

std::string sanitizeId(const std::string& s) {
    std::string out;
    for (const char c : s) {
        const char l = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
        out += (l >= 'a' && l <= 'z') || (l >= '0' && l <= '9') || l == '_' || l == '-' ? l : '_';
    }
    return out.empty() ? "game" : out;
}

Value str(const std::string& s) { return Value::string(s); }
Value num(double d) { return Value::number(d); }

Value floats(const float* p, std::size_t n) {
    Value a = Value::array();
    for (std::size_t i = 0; i < n; ++i) {
        a.push(num(double(p[i])));
    }
    return a;
}

Value matrixValue(const Mat4d& m) {
    Value a = Value::array();
    for (const double d : m) {
        a.push(num(d));
    }
    return a;
}

/// Content-addressed blob writer.
class Blobs {
public:
    explicit Blobs(fs::path root) : m_root(std::move(root)) {}
    Value put(const std::vector<std::uint8_t>& bytes, const std::string& media) {
        const std::string sha = sha256Hex(bytes.data(), bytes.size());
        if (m_written.insert(sha).second) {
            const fs::path p = m_root / "blobs" / "sha256" / sha.substr(0, 2) / sha;
            std::error_code ec;
            fs::create_directories(p.parent_path(), ec);
            if (!writeFile(p, bytes, &m_error) && m_ok) {
                m_ok = false;
            }
        }
        Value ref = Value::object();
        ref["sha256"] = str(sha);
        ref["size"] = num(double(bytes.size()));
        ref["media"] = str(media);
        return ref;
    }
    std::size_t count() const { return m_written.size(); }
    bool ok() const { return m_ok; }
    const std::string& error() const { return m_error; }

private:
    fs::path m_root;
    std::set<std::string> m_written;
    bool m_ok = true;
    std::string m_error;
};

template <typename T>
void appendLE(std::vector<std::uint8_t>& out, T v) {
    std::uint8_t b[sizeof(T)];
    std::memcpy(b, &v, sizeof(T));
    out.insert(out.end(), b, b + sizeof(T));
}

Value header(const CaptureData& c, const std::string& kind, const std::string& id, const std::string& name, const std::string& oaid) {
    Value h = Value::object();
    h["schema"] = str(kPocoSchema);
    h["kind"] = str(kind);
    h["id"] = str(id);
    h["name"] = str(name);
    Value units = Value::object();
    units["length"] = str("game");
    units["metersPerUnit"] = num(c.meta.metersPerUnit);
    units["up"] = str(c.meta.isZUp ? "+Z" : "+Y");
    units["handedness"] = str("right");
    h["units"] = units;
    h["payload"] = Value::object();
    Value prov = Value::object();
    prov["origin"] = str("original");
    prov["derived_from"] = Value::array();
    prov["recipe"] = Value();
    prov["tool"] = str("Source/FUSE/Relight/capture/export (RL-1.8 capture writer)");
    prov["ai"] = Value();
    prov["human_authorship"] = str("none");
    h["provenance"] = prov;
    h["licence_id"] = str("LicenseRef-Original-" + sanitizeId(c.meta.gameId));
    h["distribution"] = str("never");
    h["replaces"] = Value::array();
    if (!oaid.empty()) {
        h["original_asset"] = str("oaid:" + oaid); // shim extension: the original this record is
    }
    h["tags"] = Value::array();
    h["tags"].push(str("capture"));
    h["review"] = Value::object();
    h["review"]["state"] = str("draft");
    return h;
}

std::string recordPath(const std::string& kind, const std::string& id) { return "poco/" + kind + "/" + id + ".poco.json"; }

} // namespace

std::string originalAssetId(const std::string& gameId, const std::string& algo, const std::string& value) {
    return uuidString(uuidV5(oaidNamespace(), sanitizeId(gameId) + "/" + algo + ":" + value));
}

bool writeFile(const fs::path& path, const std::string& bytes, std::string* error) {
    std::error_code ec;
    if (path.has_parent_path()) {
        fs::create_directories(path.parent_path(), ec);
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!out) {
        if (error) {
            *error = "cannot write " + path.string();
        }
        return false;
    }
    return true;
}

bool writeFile(const fs::path& path, const std::vector<std::uint8_t>& bytes, std::string* error) {
    return writeFile(path, std::string(bytes.begin(), bytes.end()), error);
}

bool readFile(const fs::path& path, std::vector<std::uint8_t>& bytes) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

bool readFile(const fs::path& path, std::string& text) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

bool writePocoStore(const fs::path& storeDir, const CaptureData& c, hash::HashRule assetRule,
                    const std::map<Hash64, std::vector<std::uint8_t>>& ddsFiles, StoreReport* report, std::string* error) {
    const std::string game = sanitizeId(c.meta.gameId);
    const std::vector<CaptureKey> keys = captureKeys(c, assetRule);
    Blobs blobs(storeDir);
    StoreReport rep;
    rep.keys = keys;
    std::string firstError;
    auto emit = [&](const std::string& kind, const std::string& id, const Value& record) {
        const std::string rel = recordPath(kind, id);
        std::string err;
        if (!writeFile(storeDir / rel, json::writePretty(record), &err) && firstError.empty()) {
            firstError = err;
        }
        rep.records.push_back(rel);
    };
    auto pocoId = [&](const std::string& prim) { return "cap/" + game + "/" + prim; };

    // original_asset rows and the oaid of each Remix key.
    std::map<std::pair<std::string, std::string>, std::string> oaidOf; // (kind, remix value) -> oaid
    Value assets = Value::array();
    std::vector<std::tuple<std::string, std::string, std::string>> assetRows; // oaid, kind, first key
    auto assetFor = [&](const std::string& kind, const std::string& algo, const std::string& value) {
        const std::string oaid = originalAssetId(game, algo, value);
        oaidOf[{kind, value}] = oaid;
        assetRows.emplace_back(oaid, kind, algo + ":" + value);
        return oaid;
    };
    // Meshes.
    for (const auto& [h, mesh] : c.meshes) {
        const std::string hs = hash::hashToString(h);
        const std::string oaid = assetFor("mesh", key_algo::kGeomAsset, hs);
        const std::string prim = std::string(hash::prim_prefix::kMesh) + hs;
        Value r = header(c, "mesh", pocoId(prim), prim, oaid);
        Value& p = r["payload"];
        Value streams = Value::array();
        auto stream = [&](const char* semantic, const char* format, const std::vector<std::uint8_t>& bytes) {
            Value s = Value::object();
            s["semantic"] = str(semantic);
            s["format"] = str(format);
            s["data"] = blobs.put(bytes, "fuse/mesh-stream");
            streams.push(std::move(s));
        };
        {
            std::vector<std::uint8_t> b;
            for (const Vec3f& v : mesh.points) {
                appendLE(b, v[0]);
                appendLE(b, v[1]);
                appendLE(b, v[2]);
            }
            stream("Position", "F32x3", b);
        }
        if (!mesh.normals.empty()) {
            std::vector<std::uint8_t> b;
            for (const Vec3f& v : mesh.normals) {
                appendLE(b, v[0]);
                appendLE(b, v[1]);
                appendLE(b, v[2]);
            }
            stream("Normal", "F32x3", b);
        }
        if (!mesh.texcoords.empty()) {
            std::vector<std::uint8_t> b;
            for (const Vec2f& v : mesh.texcoords) {
                appendLE(b, v[0]);
                appendLE(b, v[1]);
            }
            stream("Uv0", "F32x2", b);
        }
        if (!mesh.colors.empty()) {
            std::vector<std::uint8_t> b;
            for (const Vec4f& v : mesh.colors) {
                for (int k = 0; k < 4; ++k) {
                    b.push_back(static_cast<std::uint8_t>(std::lround(std::clamp(v[k], 0.f, 1.f) * 255.f)));
                }
            }
            stream("Color0", "Unorm8x4", b);
        }
        if (mesh.numBones > 0 && mesh.bonesPerVertex <= 4) {
            std::vector<std::uint8_t> j, w;
            const std::size_t bpv = mesh.bonesPerVertex;
            for (std::size_t i = 0; i < mesh.points.size(); ++i) {
                for (std::size_t k = 0; k < 4; ++k) {
                    const bool in = k < bpv && i * bpv + k < mesh.jointIndices.size();
                    appendLE(j, static_cast<std::uint16_t>(in ? mesh.jointIndices[i * bpv + k] : 0));
                    const bool inw = k < bpv && i * bpv + k < mesh.jointWeights.size();
                    appendLE(w, inw ? mesh.jointWeights[i * bpv + k] : 0.f);
                }
            }
            stream("Joints0", "U16x4", j);
            stream("Weights0", "F32x4", w);
        }
        p["streams"] = std::move(streams);
        {
            std::vector<std::uint8_t> b;
            for (const std::int32_t i : mesh.indices) {
                appendLE(b, static_cast<std::uint32_t>(i));
            }
            p["indices32"] = blobs.put(b, "fuse/mesh-stream");
        }
        Value sub = Value::object();
        sub["indexOffset"] = num(0);
        sub["indexCount"] = num(double(mesh.indices.size()));
        sub["materialSlot"] = str(mesh.materialHash ? std::string(hash::prim_prefix::kMaterial) + hash::hashToString(mesh.materialHash) : "");
        p["submeshes"] = Value::array();
        p["submeshes"].push(std::move(sub));
        p["bounds"] = floats(mesh.bounds.data(), 6);
        p["skeletonId"] = str("");
        p["lodIds"] = Value::array();
        Value rl = Value::object();
        Value comps = Value::object();
        for (std::uint32_t i = 0; i < hash::kHashComponentCount; ++i) {
            const auto comp = static_cast<hash::HashComponent>(i);
            comps[hash::hashComponentName(comp)] = str(hash::hashToString(mesh.components[comp]));
        }
        rl["hash_components"] = std::move(comps);
        rl["asset_rule"] = str(c.meta.geometryHashRule);
        rl["categories"] = str(mesh.categories.toString());
        rl["double_sided"] = Value::boolean(mesh.isDoubleSided);
        rl["bones"] = num(mesh.numBones);
        rl["bones_per_vertex"] = num(mesh.bonesPerVertex);
        p["relight"] = std::move(rl);
        emit("mesh", pocoId(prim), r);
    }
    // Textures (one texture_set per captured albedo texture) and materials.
    for (const auto& [h, tex] : c.textures) {
        const std::string hs = hash::hashToString(h);
        const std::string oaid = assetFor("texture", tex.obsoleteHash ? key_algo::kTextureObsolete : key_algo::kTexture, hs);
        const std::string id = pocoId("tex_" + hs);
        Value r = header(c, "texture_set", id, "tex_" + hs, oaid);
        Value& p = r["payload"];
        if (auto it = ddsFiles.find(h); it != ddsFiles.end()) {
            p["albedo"] = blobs.put(it->second, "image/vnd-ms.dds");
        }
        p["normalConvention"] = str("GL");
        p["texelsPerMetre"] = num(0);
        p["albedoSrgb"] = Value::boolean(true);
        Value rl = Value::object();
        rl["d3d_format"] = num(tex.d3dFormat);
        rl["width"] = num(tex.width);
        rl["height"] = num(tex.height);
        rl["remix_hash"] = str(hs);
        rl["obsolete_hash"] = Value::boolean(tex.obsoleteHash);
        rl["rt_descriptor_hash"] = str(tex.descriptorHash ? hash::hashToString(tex.descriptorHash) : "");
        rl["bytes_captured"] = Value::boolean(!tex.mip0.empty());
        p["relight"] = std::move(rl);
        emit("texture_set", id, r);
    }
    for (const auto& [h, mat] : c.materials) {
        const std::string hs = hash::hashToString(h);
        const std::string prim = std::string(hash::prim_prefix::kMaterial) + hs;
        const std::string id = pocoId(prim);
        Value r = header(c, "material", id, prim, "");
        if (auto it = oaidOf.find({"texture", hs}); it != oaidOf.end()) {
            r["provenance"]["derived_from"].push(str("oaid:" + it->second));
        }
        Value& p = r["payload"];
        p["model"] = str(mat.blendEnabled ? "Translucent" : mat.alphaTestEnabled ? "Masked" : "Opaque");
        p["textureSetId"] = str(c.textures.count(h) ? pocoId("tex_" + hs) : "");
        const float white[4] = {1.f, 1.f, 1.f, 1.f};
        p["baseColor"] = floats(white, 4);
        p["roughness"] = num(0.5);
        p["metallic"] = num(0);
        p["emissiveNits"] = num(0);
        p["alphaCutoff"] = num(mat.alphaTestEnabled ? double(mat.alphaTestReferenceValue) / 255.0 : 0.5);
        p["twoSided"] = Value::boolean(false);
        p["physicalCategory"] = str("");
        p["layerRecipe"] = str("");
        emit("material", id, r);
        // relight_material (plan §4.4): the Remix fields the base Material lacks.
        Value e = header(c, "material_ext", id, prim, "");
        Value& ep = e["payload"];
        ep["enable_opacity"] = Value::boolean(mat.enableOpacity);
        ep["blend_enabled"] = Value::boolean(mat.blendEnabled);
        ep["alpha_test_enabled"] = Value::boolean(mat.alphaTestEnabled);
        ep["alpha_test_type"] = num(mat.alphaTestCompareOp);
        ep["alpha_test_reference_value"] = num(mat.alphaTestReferenceValue);
        ep["tfactor"] = num(mat.tFactor);
        ep["filter_mode"] = num(mat.filter);
        ep["wrap_mode_u"] = num(mat.wrapU);
        ep["wrap_mode_v"] = num(mat.wrapV);
        emit("material_ext", id, e);
    }
    // Lights.
    auto lightRecord = [&](Hash64 h, const char* type, const Vec3f& color, float intensity, Value relight) {
        const std::string hs = hash::hashToString(h);
        const std::string oaid = assetFor("light", key_algo::kLight, hs);
        const std::string prim = std::string(hash::prim_prefix::kLight) + hs;
        Value r = header(c, "light", pocoId(prim), prim, oaid);
        Value& p = r["payload"];
        p["type"] = str(type);
        p["colorLinear"] = floats(color.data(), 3);
        p["intensity"] = num(intensity);
        p["castsShadows"] = Value::boolean(true);
        p["relight"] = std::move(relight);
        emit("light", pocoId(prim), r);
    };
    for (const auto& [h, l] : c.sphereLights) {
        Value rl = Value::object();
        rl["radius"] = num(l.radius);
        rl["shaping"] = Value::boolean(l.shapingEnabled);
        rl["cone_angle_degrees"] = num(l.coneAngleDegrees);
        rl["cone_softness"] = num(l.coneSoftness);
        rl["focus_exponent"] = num(l.focusExponent);
        rl["intensity_units"] = str("remix");
        lightRecord(h, l.shapingEnabled ? "Spot" : "Point", l.color, l.intensity, std::move(rl));
    }
    for (const auto& [h, l] : c.distantLights) {
        Value rl = Value::object();
        rl["angle_degrees"] = num(l.angleDegrees);
        rl["direction"] = floats(l.direction.data(), 3);
        rl["intensity_units"] = str("remix");
        lightRecord(h, "Directional", l.color, l.intensity, std::move(rl));
    }
    // The level: instances, lights, camera.
    {
        const std::string id = pocoId(sanitizeId(c.meta.stageName));
        Value r = header(c, "level", id, c.meta.stageName, "");
        Value& p = r["payload"];
        Value entities = Value::array();
        for (const auto& [iid, inst] : c.instances) {
            Value e = Value::object();
            e["archetype"] = str("remix.capture.instance");
            e["transform"] = matrixValue(inst.xforms.empty() ? identity4d() : inst.xforms.front().xform);
            Value refs = Value::array();
            refs.push(str("poco:" + pocoId(std::string(hash::prim_prefix::kMesh) + hash::hashToString(inst.mesh))));
            if (inst.material != 0) {
                refs.push(str("poco:" + pocoId(std::string(hash::prim_prefix::kMaterial) + hash::hashToString(inst.material))));
            }
            e["assetRefs"] = std::move(refs);
            e["components"] = Value::array();
            e["legacyClass"] = str(inst.isSky ? "sky" : "");
            e["legacyName"] = str(inst.primName());
            entities.push(std::move(e));
        }
        p["entities"] = std::move(entities);
        Value lightIds = Value::array();
        for (const auto& [h, l] : c.sphereLights) {
            (void)l;
            lightIds.push(str(pocoId(std::string(hash::prim_prefix::kLight) + hash::hashToString(h))));
        }
        for (const auto& [h, l] : c.distantLights) {
            (void)l;
            lightIds.push(str(pocoId(std::string(hash::prim_prefix::kLight) + hash::hashToString(h))));
        }
        p["lightIds"] = std::move(lightIds);
        p["lookId"] = str("");
        p["skyId"] = str("");
        Value rl = Value::object();
        rl["frames"] = num(double(c.meta.numFramesCaptured));
        rl["geometry_hash_rule"] = str(c.meta.geometryHashRule);
        if (c.camera.valid) {
            Value cam = Value::object();
            cam["fov_radians"] = num(c.camera.fov);
            cam["aspect_ratio"] = num(c.camera.aspectRatio);
            cam["near"] = num(c.camera.nearPlane);
            cam["far"] = num(c.camera.farPlane);
            cam["view_to_world"] = matrixValue(c.camera.xforms.empty() ? identity4d() : c.camera.xforms.front().xform);
            rl["camera"] = std::move(cam);
        }
        p["relight"] = std::move(rl);
        emit("level", id, r);
    }

    // The DB tables.
    Value db = Value::object();
    db["schema"] = str(kDbSchema);
    db["game_id"] = str(game);
    std::sort(assetRows.begin(), assetRows.end());
    Value oa = Value::array();
    for (const auto& [oaid, kind, first] : assetRows) {
        Value row = Value::object();
        row["oaid"] = str(oaid);
        row["kind"] = str(kind);
        row["game_id"] = str(game);
        row["first_key"] = str(first);
        oa.push(std::move(row));
    }
    db["original_asset"] = std::move(oa);
    // Canonical keys belong to the asset of their Remix key: find it through the capture again.
    std::map<std::string, std::string> shaOwner; // sha -> oaid
    for (const auto& [h, mesh] : c.meshes) {
        shaOwner.try_emplace(canonicalMeshSha256(mesh), oaidOf[{"mesh", hash::hashToString(h)}]);
    }
    for (const auto& [h, tex] : c.textures) {
        const std::string sha = canonicalTextureSha256(tex);
        if (!sha.empty()) {
            shaOwner.try_emplace(sha, oaidOf[{"texture", hash::hashToString(h)}]);
        }
    }
    Value hk = Value::array();
    for (const CaptureKey& k : keys) {
        Value row = Value::object();
        row["algo"] = str(k.algo);
        row["value"] = str(k.value);
        std::string oaid;
        if (k.algo == key_algo::kCaptureSha256) {
            oaid = shaOwner[k.value];
        } else if (auto it = oaidOf.find({k.kind, k.value}); it != oaidOf.end() && k.algo != key_algo::kRtDescriptor) {
            oaid = it->second;
        } else if (k.algo == key_algo::kRtDescriptor) {
            for (const auto& [h, tex] : c.textures) {
                if (hash::hashToString(tex.descriptorHash) == k.value) {
                    oaid = oaidOf[{"texture", hash::hashToString(h)}];
                    break;
                }
            }
        } else {
            // Legacy geometry keys point at the mesh whose asset key they belong to.
            for (const auto& [h, mesh] : c.meshes) {
                if (hash::hashToString(mesh.legacy0) == k.value || hash::hashToString(mesh.legacy1) == k.value) {
                    oaid = oaidOf[{"mesh", hash::hashToString(h)}];
                    break;
                }
            }
        }
        row["oaid"] = str(oaid);
        row["rule_id"] = k.ruleId.empty() ? Value() : str(k.ruleId);
        row["kind"] = str(k.kind);
        hk.push(std::move(row));
    }
    db["hash_key"] = std::move(hk);
    db["replacement"] = Value::array();
    std::string err;
    if (!writeFile(storeDir / "db" / "remaster_db.json", json::writePretty(db), &err) && firstError.empty()) {
        firstError = err;
    }
    if (!blobs.ok() && firstError.empty()) {
        firstError = blobs.error();
    }
    rep.blobs = blobs.count();
    std::sort(rep.records.begin(), rep.records.end());
    if (report) {
        *report = std::move(rep);
    }
    if (!firstError.empty()) {
        if (error) {
            *error = firstError;
        }
        return false;
    }
    return true;
}

// ---- re-ingest ------------------------------------------------------------------------------------------------

IngestResult ingestPocoStore(const fs::path& storeDir) {
    IngestResult res;
    auto err = [&](const std::string& e) { res.errors.push_back(e); };
    std::string dbText;
    if (!readFile(storeDir / "db" / "remaster_db.json", dbText)) {
        err("missing db/remaster_db.json");
        return res;
    }
    std::string perr;
    const auto db = json::parse(dbText, &perr);
    if (!db || db->str("schema") != kDbSchema) {
        err("db/remaster_db.json: not a " + std::string(kDbSchema) + " document " + perr);
        return res;
    }
    const std::string game = db->str("game_id");
    std::map<std::string, std::string> assetKind;
    if (const Value* oa = db->get("original_asset")) {
        for (const Value& row : oa->a) {
            const std::string oaid = row.str("oaid");
            if (!assetKind.emplace(oaid, row.str("kind")).second) {
                err("original_asset: duplicate oaid " + oaid);
            }
            const std::string first = row.str("first_key");
            const std::size_t colon = first.find(':');
            if (colon == std::string::npos ||
                originalAssetId(game, first.substr(0, colon), first.substr(colon + 1)) != oaid) {
                err("original_asset " + oaid + ": does not recompute from its first key " + first);
            }
        }
    }
    res.assets = assetKind.size();
    std::set<std::pair<std::string, std::string>> seen;
    if (const Value* hk = db->get("hash_key")) {
        for (const Value& row : hk->a) {
            CaptureKey k;
            k.algo = row.str("algo");
            k.value = row.str("value");
            k.ruleId = row.str("rule_id");
            k.kind = row.str("kind");
            if (!seen.insert({k.algo, k.value}).second) {
                err("hash_key: duplicate (" + k.algo + ", " + k.value + ")");
            }
            const std::string oaid = row.str("oaid");
            const auto it = assetKind.find(oaid);
            if (it == assetKind.end()) {
                err("hash_key (" + k.algo + ", " + k.value + "): unknown oaid '" + oaid + "'");
            } else if (it->second != k.kind) {
                err("hash_key (" + k.algo + ", " + k.value + "): kind " + k.kind + " but the asset is a " + it->second);
            }
            if (k.algo.rfind("remix.", 0) == 0) {
                const auto parsed = hash::parseHashOption(k.value);
                if (k.value.size() != 16 || !parsed || hash::hashToString(*parsed) != k.value) {
                    err("hash_key (" + k.algo + ", " + k.value + "): not a 16-digit upper-case Remix hash");
                }
            }
            res.keys.push_back(std::move(k));
        }
    }
    std::sort(res.keys.begin(), res.keys.end());

    // POCO records and their blobs.
    std::set<std::string> blobsChecked;
    std::error_code ec;
    std::vector<fs::path> files;
    if (fs::is_directory(storeDir / "poco", ec)) {
        for (const auto& e : fs::recursive_directory_iterator(storeDir / "poco", ec)) {
            if (e.is_regular_file() && e.path().filename().string().ends_with(".poco.json")) {
                files.push_back(e.path());
            }
        }
    }
    std::sort(files.begin(), files.end());
    for (const fs::path& f : files) {
        const std::string rel = fs::relative(f, storeDir, ec).generic_string();
        std::string text;
        readFile(f, text);
        const auto rec = json::parse(text, &perr);
        if (!rec || rec->str("schema") != kPocoSchema) {
            err(rel + ": not a " + std::string(kPocoSchema) + " record");
            continue;
        }
        ++res.records;
        const std::string kind = rec->str("kind");
        if (rel != recordPath(kind, rec->str("id"))) {
            err(rel + ": path does not match kind '" + kind + "' and id '" + rec->str("id") + "'");
        }
        if (const std::string oa = rec->str("original_asset"); !oa.empty() && assetKind.count(oa.substr(5)) == 0) {
            err(rel + ": original_asset " + oa + " is not in the DB");
        }
        // Every BlobRef anywhere in the payload.
        std::vector<const Value*> stack{rec->get("payload")};
        while (!stack.empty()) {
            const Value* v = stack.back();
            stack.pop_back();
            if (!v) {
                continue;
            }
            if (v->isObject()) {
                if (v->get("sha256") && v->get("media")) {
                    const std::string sha = v->str("sha256");
                    std::vector<std::uint8_t> bytes;
                    if (sha.size() != 64 || !readFile(storeDir / "blobs" / "sha256" / sha.substr(0, 2) / sha, bytes)) {
                        err(rel + ": missing blob " + sha);
                        continue;
                    }
                    if (bytes.size() != static_cast<std::size_t>(v->num("size", -1)) || sha256Hex(bytes.data(), bytes.size()) != sha) {
                        err(rel + ": blob " + sha + " has the wrong size or content");
                    }
                    blobsChecked.insert(sha);
                    if (v->str("media") == "image/vnd-ms.dds") {
                        std::string derr;
                        const auto img = readDds(bytes, &derr);
                        const Value* rl = rec->get("payload") ? rec->get("payload")->get("relight") : nullptr;
                        if (!img || img->mips.empty()) {
                            err(rel + ": DDS blob does not read back: " + derr);
                        } else if (rl) {
                            const bool obsolete = rl->flag("obsolete_hash");
                            const Hash64 h = obsolete ? hash::hashTextureMip0Obsolete(img->mips[0].data(), img->mips[0].size())
                                                      : hash::hashTextureMip0(img->mips[0].data(), img->mips[0].size());
                            if (hash::hashToString(h) != rl->str("remix_hash")) {
                                err(rel + ": DDS mip 0 hashes to " + hash::hashToString(h) + ", the record says " + rl->str("remix_hash"));
                            }
                        }
                    }
                    continue;
                }
                for (const auto& kv : v->o) {
                    stack.push_back(&kv.second);
                }
            } else if (v->isArray()) {
                for (const Value& e : v->a) {
                    stack.push_back(&e);
                }
            }
        }
    }
    res.blobs = blobsChecked.size();
    return res;
}

} // namespace fuse::relight::capture::exporter
