// FUSE Relight RL-1.8 tests: re-ingest and structural checks of a written capture directory.
//
// checkCapture(dir) re-reads both capture forms without the writer's in-memory state:
//   * the FUSE capture (store/): ingestPocoStore (every record, blob, DDS and DB row), giving its key set;
//   * the Remix-compatible capture: every .usda file parses with the minimal parser (usda_mini_parser.hpp);
//     the instance stage has the §1.9 layout (/RootNode with lights, meshes, Looks, instances, cameras,
//     customLayerData lightspeed_layer_type = "capture" and the geometry hash rule); every reference
//     resolves (file + prim); each mesh_<H> has a triangle-list UsdGeomMesh (faceVertexCounts all 3,
//     indices inside points, per-vertex primvars sized to the points) and a hash component customLayerData
//     whose asset-rule combination gives <H> again; each mat_<H> points at textures/<H>.dds, which reads
//     back and XXH3-hashes to <H>; instances reference existing meshes and materials and carry a transform;
//     sphere lights resolve to a SphereLight layer;
//   * the key set the USDA implies (mesh_ names -> remix.geom.asset, light_ names -> remix.light, material
//     texture files -> remix.tex) equals the store's rows of those algorithms.
#pragma once

#include "usda_mini_parser.hpp"

#include <fuse/relight/capture/export/capture_model.hpp>
#include <fuse/relight/capture/export/dds.hpp>
#include <fuse/relight/capture/export/poco_store.hpp>
#include <fuse/relight/hash/hash_string.hpp>
#include <fuse/relight/hash/texture_hash.hpp>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

namespace rl_capture_check {

namespace fs = std::filesystem;
namespace ex = fuse::relight::capture::exporter;
namespace rh = fuse::relight::hash;

struct Result {
    std::vector<std::string> errors;
    std::vector<ex::CaptureKey> storeKeys;
    std::set<std::pair<std::string, std::string>> usdKeys; ///< (algo, value) implied by the USDA
    std::size_t layers = 0, meshes = 0, materials = 0, instances = 0, lights = 0, textures = 0;
    bool camera = false;
    bool ok() const { return errors.empty(); }
};

inline std::size_t arraySize(const rl_usda::Property* p) {
    if (!p) {
        return 0;
    }
    if (p->hasDefault) {
        return p->value.items.size();
    }
    return p->timeSamples.empty() ? 0 : p->timeSamples.front().second.items.size();
}

inline Result checkCapture(const fs::path& dir, const std::string& stageFile, const std::string& assetRuleString) {
    Result r;
    auto err = [&](const std::string& e) { r.errors.push_back(e); };

    // ---- the FUSE capture --------------------------------------------------------------------------------
    const ex::IngestResult ingest = ex::ingestPocoStore(dir / "store");
    for (const std::string& e : ingest.errors) {
        err("store: " + e);
    }
    r.storeKeys = ingest.keys;

    // ---- every USDA layer parses ----------------------------------------------------------------------
    std::map<std::string, rl_usda::Layer> layers; // relative path -> layer
    std::error_code ec;
    for (const auto& e : fs::recursive_directory_iterator(dir, ec)) {
        if (!e.is_regular_file() || e.path().extension() != ".usda") {
            continue;
        }
        const std::string rel = fs::relative(e.path(), dir, ec).generic_string();
        std::string text;
        ex::readFile(e.path(), text);
        std::string perr;
        auto layer = rl_usda::parse(text, &perr);
        if (!layer) {
            err(rel + ": " + perr);
            continue;
        }
        layers.emplace(rel, std::move(*layer));
    }
    r.layers = layers.size();
    auto layerAt = [&](const std::string& fromDir, const std::string& asset) -> const rl_usda::Layer* {
        const std::string rel = (fs::path(fromDir) / asset).lexically_normal().generic_string();
        const auto it = layers.find(rel);
        return it == layers.end() ? nullptr : &it->second;
    };
    const auto stageIt = layers.find(stageFile);
    if (stageIt == layers.end()) {
        err("missing instance stage " + stageFile);
        return r;
    }
    const rl_usda::Layer& stage = stageIt->second;
    const rl_usda::Value* dp = stage.meta("defaultPrim");
    if (!dp || dp->s != "RootNode") {
        err("stage: defaultPrim is not RootNode");
    }
    const rl_usda::Value* cld = stage.meta("customLayerData");
    const rl_usda::Value* lt = cld ? cld->get("lightspeed_layer_type") : nullptr;
    if (!lt || lt->s != "capture") {
        err("stage: customLayerData.lightspeed_layer_type != \"capture\"");
    }
    const rl_usda::Value* rule = cld ? cld->get("lightspeed_geometry_hash_rules") : nullptr;
    if (!rule || rule->s != assetRuleString) {
        err("stage: lightspeed_geometry_hash_rules != '" + assetRuleString + "'");
    }
    const rl_usda::Prim* root = stage.find("/RootNode");
    if (!root) {
        err("stage: no /RootNode");
        return r;
    }
    for (const char* scope : {"lights", "meshes", "Looks", "instances", "cameras"}) {
        if (!root->child(scope)) {
            err(std::string("stage: no /RootNode/") + scope);
        }
    }
    const rh::HashRule assetRule = rh::parseHashRule(assetRuleString);

    // ---- materials ------------------------------------------------------------------------------------
    if (const rl_usda::Prim* looks = root->child("Looks")) {
        for (const rl_usda::Prim& m : looks->children) {
            ++r.materials;
            const rh::Hash64 h = rh::hashFromPrimName(m.name, rh::prim_prefix::kMaterial);
            if (h == 0 || rh::primName(rh::prim_prefix::kMaterial, h) != m.name) {
                err("Looks/" + m.name + ": not mat_<16 hex digits>");
                continue;
            }
            const rl_usda::Value* ref = m.meta("prepend references");
            const rl_usda::Layer* ml = ref ? layerAt(".", ref->s) : nullptr;
            const rl_usda::Prim* mp = ml ? ml->find(ref->target) : nullptr;
            if (!mp || mp->type != "Material") {
                err("Looks/" + m.name + ": reference does not resolve to a Material");
                continue;
            }
            const rl_usda::Prim* shader = mp->child("Shader");
            const rl_usda::Property* src = shader ? shader->property("info:mdl:sourceAsset") : nullptr;
            const rl_usda::Property* tex = shader ? shader->property("inputs:diffuse_texture") : nullptr;
            if (!src || src->value.s != "./AperturePBR_Opacity.mdl" || !tex) {
                err("Looks/" + m.name + ": no AperturePBR_Opacity shader with a diffuse_texture");
                continue;
            }
            const rl_usda::Property* out = mp->property("outputs:mdl:surface");
            if (!out || out->connect != ref->target + "/Shader.outputs:out") {
                err("Looks/" + m.name + ": outputs:mdl:surface is not connected to the shader");
            }
            const fs::path texPath = (dir / "materials" / tex->value.s).lexically_normal();
            if (texPath.filename() != rh::hashToString(h) + ".dds") {
                err("Looks/" + m.name + ": diffuse_texture " + tex->value.s + " is not textures/<hash>.dds");
            }
            std::vector<std::uint8_t> bytes;
            if (!ex::readFile(texPath, bytes)) {
                // A material whose texture bytes were not captured (a render target): keyed by name only.
                r.usdKeys.insert({ex::key_algo::kTexture, rh::hashToString(h)});
                continue;
            }
            ++r.textures;
            std::string derr;
            const auto img = ex::readDds(bytes, &derr);
            if (!img) {
                err(texPath.generic_string() + ": " + derr);
                continue;
            }
            const rh::Hash64 th = rh::hashTextureMip0(img->mips[0].data(), img->mips[0].size());
            const rh::Hash64 tho = rh::hashTextureMip0Obsolete(img->mips[0].data(), img->mips[0].size());
            if (th == h) {
                r.usdKeys.insert({ex::key_algo::kTexture, rh::hashToString(h)});
            } else if (tho == h) {
                r.usdKeys.insert({ex::key_algo::kTextureObsolete, rh::hashToString(h)});
            } else {
                err(texPath.generic_string() + ": mip 0 hashes to " + rh::hashToString(th) + ", not " + rh::hashToString(h));
            }
        }
    }

    // ---- meshes -----------------------------------------------------------------------------------------
    std::set<std::string> meshPrims;
    if (const rl_usda::Prim* meshes = root->child("meshes")) {
        for (const rl_usda::Prim& m : meshes->children) {
            ++r.meshes;
            const rh::Hash64 h = rh::hashFromPrimName(m.name, rh::prim_prefix::kMesh);
            if (h == 0 || rh::primName(rh::prim_prefix::kMesh, h) != m.name) {
                err("meshes/" + m.name + ": not mesh_<16 hex digits>");
                continue;
            }
            meshPrims.insert(m.name);
            r.usdKeys.insert({ex::key_algo::kGeomAsset, rh::hashToString(h)});
            const rl_usda::Property* vis = m.property("visibility");
            if (!vis || vis->value.s != "invisible") {
                err("meshes/" + m.name + ": the prototype is not invisible");
            }
            if (const rl_usda::Property* b = m.property("material:binding")) {
                if (!stage.find(b->value.s)) {
                    err("meshes/" + m.name + ": material binding " + b->value.s + " does not exist");
                }
            }
            const rl_usda::Value* ref = m.meta("prepend references");
            const rl_usda::Layer* ml = ref ? layerAt(".", ref->s) : nullptr;
            const rl_usda::Prim* mx = ml ? ml->find(ref->target) : nullptr;
            const rl_usda::Prim* geo = mx ? mx->child("mesh") : nullptr;
            if (!geo || geo->type != "Mesh") {
                err("meshes/" + m.name + ": reference does not resolve to <mesh_H>/mesh");
                continue;
            }
            // The hash components give the name back under the asset rule.
            const rl_usda::Value* comps = ml->meta("customLayerData");
            rh::GeometryHashes g;
            bool haveAll = comps != nullptr;
            for (std::uint32_t i = 0; comps && i < rh::kHashComponentCount; ++i) {
                const auto c = static_cast<rh::HashComponent>(i);
                const rl_usda::Value* v = comps->get(std::string(rh::hashComponentName(c)));
                if (!v) {
                    haveAll = false;
                    continue;
                }
                // uint64 values: read the literal text exactly (a double cannot hold 64 bits).
                g[c] = std::strtoull(v->s.c_str(), nullptr, 10);
            }
            if (!haveAll) {
                err("meshes/" + m.name + ": the mesh layer lacks the hash component customLayerData");
            } else if (g.hashForRule(assetRule) != h) {
                err("meshes/" + m.name + ": the hash components combine to " + rh::hashToString(g.hashForRule(assetRule)) +
                    " under '" + assetRuleString + "'");
            }
            const std::size_t points = arraySize(geo->property("points"));
            const rl_usda::Property* fvi = geo->property("faceVertexIndices");
            const rl_usda::Property* fvc = geo->property("faceVertexCounts");
            if (points == 0 || !fvi || !fvc || fvi->value.items.size() != fvc->value.items.size() * 3 ||
                fvi->value.items.size() % 3 != 0) {
                err("meshes/" + m.name + ": not a triangle list (points " + std::to_string(points) + ")");
                continue;
            }
            for (const rl_usda::Value& c : fvc->value.items) {
                if (c.n != 3.0) {
                    err("meshes/" + m.name + ": a face is not a triangle");
                    break;
                }
            }
            for (const rl_usda::Value& i : fvi->value.items) {
                if (i.n < 0 || i.n >= double(points)) {
                    err("meshes/" + m.name + ": index " + std::to_string(i.n) + " outside the points");
                    break;
                }
            }
            for (const char* pv : {"normals", "primvars:st", "primvars:displayColor"}) {
                const std::size_t n = arraySize(geo->property(pv));
                if (n != 0 && n != points && n != 1) {
                    err("meshes/" + m.name + ": " + pv + " has " + std::to_string(n) + " values for " + std::to_string(points) + " points");
                }
            }
            if (!geo->property("remix_category:sky")) {
                err("meshes/" + m.name + ": no remix_category:* flags");
            }
        }
    }

    // ---- instances --------------------------------------------------------------------------------------
    if (const rl_usda::Prim* insts = root->child("instances")) {
        for (const rl_usda::Prim& i : insts->children) {
            ++r.instances;
            const rl_usda::Value* ref = i.meta("prepend references");
            if (!ref || ref->kind != rl_usda::Value::Kind::Path || !stage.find(ref->s)) {
                err("instances/" + i.name + ": no internal reference to an existing mesh");
                continue;
            }
            const std::string meshName = ref->s.substr(ref->s.rfind('/') + 1);
            const std::string prefix = (i.name.rfind("sky_", 0) == 0 ? "sky_" : "inst_") + meshName.substr(5) + "_";
            if (i.name.rfind(prefix, 0) != 0) {
                err("instances/" + i.name + ": name does not follow inst_<mesh hash>_<n>");
            }
            const rl_usda::Property* x = i.property("xformOp:transform");
            if (!x || (!x->hasDefault && x->timeSamples.empty())) {
                err("instances/" + i.name + ": no xformOp:transform");
            }
            if (const rl_usda::Property* b = i.property("material:binding"); b && !stage.find(b->value.s)) {
                err("instances/" + i.name + ": material binding " + b->value.s + " does not exist");
            }
        }
    }

    // ---- lights -----------------------------------------------------------------------------------------
    if (const rl_usda::Prim* lights = root->child("lights")) {
        for (const rl_usda::Prim& l : lights->children) {
            ++r.lights;
            const rh::Hash64 h = rh::hashFromPrimName(l.name, rh::prim_prefix::kLight);
            if (h == 0 || rh::primName(rh::prim_prefix::kLight, h) != l.name) {
                err("lights/" + l.name + ": not light_<16 hex digits>");
                continue;
            }
            r.usdKeys.insert({ex::key_algo::kLight, rh::hashToString(h)});
            if (l.type == "SphereLight") {
                const rl_usda::Value* ref = l.meta("prepend references");
                const rl_usda::Layer* ll = ref ? layerAt(".", ref->s) : nullptr;
                const rl_usda::Value* def = ll ? ll->meta("defaultPrim") : nullptr;
                const rl_usda::Prim* lp = def ? ll->find("/" + def->s) : nullptr;
                if (!lp || lp->type != "SphereLight" || !lp->property("inputs:radius") || !lp->property("inputs:intensity")) {
                    err("lights/" + l.name + ": reference does not resolve to a SphereLight");
                }
            } else if (l.type == "DistantLight") {
                if (!l.property("inputs:angle") || !l.property("inputs:intensity")) {
                    err("lights/" + l.name + ": DistantLight without angle / intensity");
                }
            } else {
                err("lights/" + l.name + ": unexpected light type " + l.type);
            }
        }
    }
    if (const rl_usda::Prim* cams = root->child("cameras")) {
        if (const rl_usda::Prim* cam = cams->child("Camera")) {
            r.camera = cam->type == "Camera" && cam->property("focalLength") && cam->property("clippingRange");
            if (!r.camera) {
                err("cameras/Camera: not a Camera with focal length and clipping range");
            }
        }
    }

    // ---- key sets agree -----------------------------------------------------------------------------------
    std::set<std::pair<std::string, std::string>> storeUsdAlgos;
    for (const ex::CaptureKey& k : r.storeKeys) {
        if (k.algo == ex::key_algo::kGeomAsset || k.algo == ex::key_algo::kLight || k.algo == ex::key_algo::kTexture ||
            k.algo == ex::key_algo::kTextureObsolete) {
            storeUsdAlgos.insert({k.algo, k.value});
        }
    }
    if (storeUsdAlgos != r.usdKeys) {
        for (const auto& k : storeUsdAlgos) {
            if (!r.usdKeys.count(k)) {
                err("key (" + k.first + ", " + k.second + ") is in the store but not in the USDA");
            }
        }
        for (const auto& k : r.usdKeys) {
            if (!storeUsdAlgos.count(k)) {
                err("key (" + k.first + ", " + k.second + ") is in the USDA but not in the store");
            }
        }
    }
    return r;
}

} // namespace rl_capture_check
