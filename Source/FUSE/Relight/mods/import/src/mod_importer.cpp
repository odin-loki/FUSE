// FUSE Relight RL-3.2: the mod importer (see mod_importer.hpp).
#include <fuse/relight/mods/import/mod_importer.hpp>

#include <fuse/relight/mods/import/light_table.hpp>
#include <fuse/relight/mods/import/material_table.hpp>
#include <fuse/relight/mods/import/mesh_import.hpp>

#include <fuse/relight/capture/export/capture_model.hpp>
#include <fuse/relight/capture/export/dds.hpp>
#include <fuse/relight/capture/export/digest.hpp>
#include <fuse/relight/capture/export/poco_store.hpp>
#include <fuse/relight/hash/geometry_hash.hpp>
#include <fuse/relight/hash/hash_string.hpp>
#include <fuse/relight/mods/assets/asset_package.hpp>
#include <fuse/relight/mods/assets/dds.hpp>
#include <fuse/relight/mods/usd/remix_profile.hpp>
#include <fuse/relight/scene/classify/instance_categories.hpp>

#include <algorithm>
#include <functional>
#include <set>
#include <tuple>

namespace fuse::relight::mods::import {

namespace fs = std::filesystem;
namespace ex = capture::exporter;
using hash::Hash64;
using json::Value;

namespace {

Value str(const std::string& s) { return Value::string(s); }
Value num(double d) { return Value::number(d); }

std::string lower(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return s;
}

/// RL-1.8's game id sanitizer (lower case, [a-z0-9_-]).
std::string sanitizeGame(const std::string& s) {
    std::string out;
    for (const char c : lower(s)) {
        out += (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-' ? c : '_';
    }
    return out.empty() ? "game" : out;
}

/// One id path segment: [A-Za-z0-9_.-], never "." or "..".
std::string sanitizeSegment(const std::string& s) {
    std::string out;
    for (const char c : s) {
        out += (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.'
                   ? c
                   : '_';
    }
    if (out.empty() || out == "." || out == "..") {
        out = "_" + out;
    }
    return out;
}

/// A prim path below `base` as id segments ("/RootNode/meshes/mesh_A/x" below "/RootNode/meshes" -> "mesh_A/x").
std::string idFromPath(const std::string& path, const std::string& base) {
    std::string rest = path;
    if (!base.empty() && path.compare(0, base.size() + 1, base + "/") == 0) {
        rest = path.substr(base.size() + 1);
    } else if (!rest.empty() && rest[0] == '/') {
        rest = rest.substr(1);
    }
    std::string out;
    std::size_t b = 0;
    while (b <= rest.size()) {
        std::size_t e = rest.find('/', b);
        if (e == std::string::npos) {
            e = rest.size();
        }
        if (!out.empty()) {
            out += '/';
        }
        out += sanitizeSegment(rest.substr(b, e - b));
        b = e + 1;
    }
    return out;
}

std::vector<std::string> splitPath(const std::string& p) {
    std::vector<std::string> parts;
    std::size_t b = 0;
    while (b < p.size()) {
        std::size_t e = p.find('/', b);
        if (e == std::string::npos) {
            e = p.size();
        }
        if (e > b) {
            parts.push_back(p.substr(b, e - b));
        }
        b = e + 1;
    }
    return parts;
}

/// Lexical relative path of `p` from directory `dir` (both normalized, '/').
std::string relativeTo(const std::string& dir, const std::string& p) {
    if (p == dir) {
        return ".";
    }
    if (!dir.empty() && p.compare(0, dir.size() + 1, dir + "/") == 0) {
        return p.substr(dir.size() + 1);
    }
    const bool absA = !dir.empty() && dir[0] == '/', absB = !p.empty() && p[0] == '/';
    if (absA != absB) {
        return p;
    }
    const auto a = splitPath(dir), b = splitPath(p);
    std::size_t common = 0;
    while (common < a.size() && common < b.size() && a[common] == b[common]) {
        ++common;
    }
    if (common == 0 && (absA || (!a.empty() && a[0].find(':') != std::string::npos))) {
        return p; // different drives / roots
    }
    std::string out;
    for (std::size_t i = common; i < a.size(); ++i) {
        out += "../";
    }
    for (std::size_t i = common; i < b.size(); ++i) {
        out += b[i];
        if (i + 1 < b.size()) {
            out += '/';
        }
    }
    return out.empty() ? "." : out;
}

std::string baseName(const std::string& p) {
    const std::size_t slash = p.rfind('/');
    return slash == std::string::npos ? p : p.substr(slash + 1);
}

std::string trim(const std::string& s) {
    const std::size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) {
        return {};
    }
    const std::size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

std::vector<std::uint8_t> bytesOf(const std::string& s) { return {s.begin(), s.end()}; }

Value matrixJson(const Mat4d& m) {
    Value a = Value::array();
    for (double d : m) {
        a.push(num(d == 0.0 ? 0.0 : d)); // no negative zero in the records
    }
    return a;
}

std::string recordPath(const std::string& kind, const std::string& id) { return "poco/" + kind + "/" + id + ".poco.json"; }

bool isBlobRef(const Value& v) { return v.isObject() && v.get("sha256") && v.get("media"); }

// ---- the import context ----------------------------------------------------------------------------------------

struct KeyRow {
    std::string algo, value, oaid, ruleId, kind;
};
struct AssetRow {
    std::string oaid, kind, firstKey;
};
struct ReplacementRow {
    std::string oaid, variant, tier, pocoId, source, recordKind;
};

class Importer {
public:
    Importer(const ImportOptions& o, ImportResult& r) : opt_(o), res_(r) {}

    void run();

private:
    // Inputs.
    const ImportOptions& opt_;
    ImportResult& res_;
    usd::FileSource files_;
    std::string modDir_, stagePath_, name_, game_, prefix_;
    bool capture_ = false;
    usd::ComposedStage stage_;
    usd::RemixMod remix_;
    hash::HashRule rule_{};
    std::string ruleString_, ruleSource_, geomAlgo_, ruleId_;
    std::string licenceId_;
    Value units_;
    // Outputs.
    std::set<std::string> blobShas_;
    std::map<std::string, std::string> materialIds_; ///< material prim path -> poco id ("" while importing)
    std::vector<AssetRow> assets_;
    std::vector<ex::CaptureKey> captureKeys_; ///< capture: every key (sorted + deduplicated like RL-1.8)
    std::vector<KeyRow> keys_;
    std::vector<ReplacementRow> replacements_;
    std::map<std::string, std::string> meshShaOwner_, textureShaOwner_; ///< capture: sha -> oaid (first by hash)
    std::vector<std::pair<Hash64, std::string>> captureMeshShas_, captureTextureShas_;
    // Packages.
    struct Package {
        std::string rel;
        std::optional<assets::AssetPackage> pkg;
        std::map<std::string, std::uint32_t> index; ///< normalized asset name -> index
    };
    std::vector<Package> packages_;
    bool packagesLoaded_ = false;

    void diag(const char* severity, const std::string& code, const std::string& where, const std::string& message) {
        res_.diagnostics.push_back({severity, code, where, message});
    }
    void warn(const std::string& code, const std::string& where, const std::string& message) {
        diag("warning", code, where, message);
    }
    void error(const std::string& code, const std::string& where, const std::string& message) {
        diag("error", code, where, message);
    }
    std::string rel(const std::string& p) const { return relativeTo(modDir_, usd::normalizePath(p)); }
    std::optional<std::string> readText(const std::string& path) const { return files_(path); }

    Value blob(const std::vector<std::uint8_t>& bytes, const std::string& media) {
        const std::string sha = ex::sha256Hex(bytes.data(), bytes.size());
        if (blobShas_.insert(sha).second) {
            res_.files["blobs/sha256/" + sha.substr(0, 2) + "/" + sha] = bytes;
        }
        Value ref = Value::object();
        ref["sha256"] = str(sha);
        ref["size"] = num(double(bytes.size()));
        ref["media"] = str(media);
        return ref;
    }

    void emit(const std::string& kind, const std::string& id, const Value& record) {
        res_.files[recordPath(kind, id)] = bytesOf(json::writePretty(record));
        ++res_.counts.records;
    }

    Value header(const std::string& kind, const std::string& id, const std::string& name, const std::string& prim,
                 const std::string& originalOaid, const std::vector<std::string>& replaces) const {
        Value h = Value::object();
        h["schema"] = str(ex::kPocoSchema);
        h["kind"] = str(kind);
        h["id"] = str(id);
        h["name"] = str(name);
        h["units"] = units_;
        h["payload"] = Value::object();
        Value prov = Value::object();
        prov["origin"] = str(capture_ ? "original" : "derived");
        Value from = Value::array();
        if (!capture_) {
            from.push(str("mod:" + name_));
        }
        prov["derived_from"] = std::move(from);
        prov["recipe"] = Value();
        prov["tool"] = str(kImportTool);
        prov["ai"] = Value();
        prov["human_authorship"] = str(capture_ ? "none" : "unknown");
        Value src = Value::object(); // shim extension: where in the mod the record came from
        src["mod"] = str(name_);
        src["stage"] = str(rel(stagePath_));
        src["prim"] = str(prim);
        if (const usd::Prim* p = prim.empty() ? nullptr : stage_.find(prim); p && !p->stack.empty()) {
            src["layer"] = str(rel(p->stack.front().layer));
        }
        prov["source"] = std::move(src);
        h["provenance"] = std::move(prov);
        h["licence_id"] = str(licenceId_);
        h["distribution"] = str("never");
        Value rep = Value::array();
        for (const std::string& o : replaces) {
            rep.push(str("oaid:" + o));
        }
        h["replaces"] = std::move(rep);
        if (!originalOaid.empty()) {
            h["original_asset"] = str("oaid:" + originalOaid);
        }
        Value tags = Value::array();
        tags.push(str(capture_ ? "capture" : "mod"));
        tags.push(str("remix"));
        h["tags"] = std::move(tags);
        h["review"] = Value::object();
        h["review"]["state"] = str("draft");
        return h;
    }

    std::string originalAsset(const std::string& kind, const std::string& algo, const std::string& value,
                              const std::string& ruleId) {
        const std::string oaid = ex::originalAssetId(game_, algo, value);
        assets_.push_back({oaid, kind, algo + ":" + value});
        keys_.push_back({algo, value, oaid, ruleId, kind});
        if (capture_) {
            captureKeys_.push_back({algo, value, ruleId, kind});
        }
        return oaid;
    }

    bool resolveStage();
    void readModConfig(Value& modPayload);
    void detectLicence(Value& modPayload);
    void loadPackages();
    std::optional<std::vector<std::uint8_t>> packagedTexture(const std::string& relPath, std::string* source);
    Value importTexture(const MaterialParams& params, const std::string& param, const usd::Attribute* attr,
                        std::vector<std::uint8_t>* ddsBytes);
    std::string importMaterial(const std::string& path, Hash64 remixHash);
    std::string importParticles(const std::string& ownerId, const usd::Prim& prim, const usd::ParticleSystem& ps);
    std::string importLight(const usd::Prim& prim, const std::string& id, const Mat4d& transform, Hash64 remixHash,
                            const std::string& attachedTo);
    void importMeshReplacement(const usd::MeshReplacement& r);
    Value categoriesJson(const usd::CategoryOverrides& c) const;
    Value buildDb();
};

bool Importer::resolveStage() {
    std::string root = usd::normalizePath(opt_.root);
    while (root.size() > 1 && root.back() == '/') {
        root.pop_back();
    }
    const std::size_t dot = root.rfind('.');
    const std::size_t slash = root.rfind('/');
    const std::string ext = dot != std::string::npos && (slash == std::string::npos || dot > slash) ? lower(root.substr(dot)) : "";
    if (ext == ".usda" || ext == ".usdc" || ext == ".usd") {
        stagePath_ = root;
        modDir_ = usd::parentDir(root);
        if (modDir_.empty()) {
            modDir_ = ".";
        }
    } else {
        modDir_ = root;
        for (const char* n : {"mod.usda", "mod.usdc", "mod.usd"}) {
            if (files_(modDir_ + "/" + n)) {
                stagePath_ = modDir_ + "/" + n;
                break;
            }
        }
        if (stagePath_.empty()) {
            error("no_mod_layer", ".", "no mod.usda / mod.usdc / mod.usd in " + root);
            return false;
        }
    }
    return true;
}

void Importer::readModConfig(Value& modPayload) {
    modPayload["rtx_conf"] = Value();
    std::string confRule;
    if (auto text = readText(modDir_ + "/rtx.conf")) {
        modPayload["rtx_conf"] = blob(bytesOf(*text), "text/plain");
        std::size_t b = 0;
        while (b < text->size()) {
            std::size_t e = text->find('\n', b);
            if (e == std::string::npos) {
                e = text->size();
            }
            const std::string line = trim(text->substr(b, e - b));
            b = e + 1;
            if (line.empty() || line[0] == '#') {
                continue;
            }
            const std::size_t eq = line.find('=');
            if (eq == std::string::npos) {
                continue;
            }
            const std::string key = trim(line.substr(0, eq));
            if (key == "rtx.geometryAssetHashRuleString" || key == "relight.geometryAssetHashRuleString") {
                confRule = trim(line.substr(eq + 1));
            }
        }
    }
    std::string layerRule;
    if (const usd::Value* v = stage_.customLayerData.get("lightspeed_geometry_hash_rules")) {
        layerRule = v->asString().value_or("");
    }
    if (!opt_.assetRule.empty()) {
        ruleString_ = opt_.assetRule;
        ruleSource_ = "option";
    } else if (!confRule.empty()) {
        ruleString_ = confRule;
        ruleSource_ = "rtx.conf";
    } else if (!layerRule.empty()) {
        ruleString_ = layerRule;
        ruleSource_ = "layer";
    } else {
        ruleString_ = std::string(hash::rules::kDefaultAssetRuleString);
        ruleSource_ = "default";
    }
    rule_ = hash::parseHashRule(ruleString_);
    if (rule_.bits == 0) {
        warn("bad_hash_rule", ".", "geometry asset rule '" + ruleString_ + "' selects no component; using the default");
        rule_ = hash::parseHashRule(hash::rules::kDefaultAssetRuleString);
        ruleSource_ = "default";
    }
    ruleString_ = hash::formatHashRule(rule_);
    geomAlgo_ = rule_ == hash::rules::kLegacyAsset0   ? ex::key_algo::kGeomLegacy0
                : rule_ == hash::rules::kLegacyAsset1 ? ex::key_algo::kGeomLegacy1
                                                      : ex::key_algo::kGeomAsset;
    ruleId_ = hash::hashToString(hash::hashRuleId(rule_));
    modPayload["geometry_asset_rule"] = str(ruleString_);
    modPayload["geometry_asset_rule_source"] = str(ruleSource_);
}

void Importer::detectLicence(Value& modPayload) {
    Value lic = Value::object();
    if (capture_) {
        licenceId_ = "LicenseRef-Original-" + game_;
        lic["id"] = str(licenceId_);
        lic["file"] = Value();
        lic["text"] = Value();
        lic["detected"] = Value::boolean(false);
        modPayload["licence"] = std::move(lic);
        return;
    }
    std::string file, text;
    for (const char* stem : {"LICENSE", "LICENCE", "License", "Licence", "license", "licence", "COPYING", "Copying", "copying"}) {
        for (const char* ext : {"", ".txt", ".md"}) {
            const std::string candidate = std::string(stem) + ext;
            if (auto t = readText(modDir_ + "/" + candidate)) {
                file = candidate;
                text = std::move(*t);
                break;
            }
        }
        if (!file.empty()) {
            break;
        }
    }
    const std::string spdx = file.empty() ? std::string() : detectSpdxLicence(text);
    licenceId_ = !spdx.empty() ? spdx : "LicenseRef-ThirdPartyMod-" + sanitizeSegment(name_);
    lic["id"] = str(licenceId_);
    lic["file"] = file.empty() ? Value() : str(file);
    lic["text"] = file.empty() ? Value() : blob(bytesOf(text), "text/plain");
    lic["detected"] = Value::boolean(!spdx.empty());
    if (file.empty()) {
        warn("no_licence_file", ".", "the mod has no licence file; records use " + licenceId_ + " and are never shipped");
    } else if (spdx.empty()) {
        warn("unknown_licence", file, "licence text not recognised; records use " + licenceId_ + " and are never shipped");
    }
    modPayload["licence"] = std::move(lic);
}

void Importer::loadPackages() {
    if (packagesLoaded_) {
        return;
    }
    packagesLoaded_ = true;
    std::vector<std::string> names;
    if (opt_.packages) {
        names = *opt_.packages;
    } else if (!opt_.files) {
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(fs::path(modDir_), ec)) {
            const std::string ext = lower(e.path().extension().string());
            if (e.is_regular_file(ec) && (ext == ".pkg" || ext == ".rtxio")) {
                names.push_back(e.path().filename().generic_string());
            }
        }
    }
    std::sort(names.begin(), names.end());
    std::reverse(names.begin(), names.end()); // upstream: reverse alphabetical order
    for (const std::string& n : names) {
        Package p;
        p.rel = n;
        std::string err;
        if (opt_.files) {
            if (auto bytes = files_(modDir_ + "/" + n)) {
                p.pkg = assets::AssetPackage::fromBytes(std::span(reinterpret_cast<const std::uint8_t*>(bytes->data()), bytes->size()), &err);
            }
        } else {
            p.pkg = assets::AssetPackage::open(fs::path(modDir_) / n, &err);
        }
        if (!p.pkg) {
            warn("bad_package", n, "package cannot be read: " + err);
            continue;
        }
        for (std::uint32_t i = 0; i < p.pkg->assetCount(); ++i) {
            std::string key = lower(p.pkg->assetName(i));
            std::replace(key.begin(), key.end(), '\\', '/');
            while (key.compare(0, 2, "./") == 0) {
                key = key.substr(2);
            }
            p.index.emplace(std::move(key), i); // first match wins, as upstream findAsset
        }
        packages_.push_back(std::move(p));
    }
}

std::optional<std::vector<std::uint8_t>> Importer::packagedTexture(const std::string& relPath, std::string* source) {
    loadPackages();
    const std::string key = lower(relPath);
    for (const Package& p : packages_) {
        const auto it = p.index.find(key);
        if (it == p.index.end()) {
            continue;
        }
        std::string err;
        const auto image = p.pkg->loadImage(it->second, &err);
        if (!image) {
            warn("bad_package_asset", p.rel, relPath + ": " + err);
            return std::nullopt;
        }
        auto bytes = assets::writeDds(*image, &err);
        if (bytes.empty()) {
            warn("bad_package_asset", p.rel, relPath + ": " + err);
            return std::nullopt;
        }
        *source = "package:" + p.rel + "#" + p.pkg->assetName(it->second);
        return bytes;
    }
    return std::nullopt;
}

Value Importer::importTexture(const MaterialParams& params, const std::string& param, const usd::Attribute* attr,
                              std::vector<std::uint8_t>* ddsBytes) {
    Value x = Value::object();
    const ParamValue& pv = params.values.at(param);
    const std::string resolved = attr && attr->hasDefault ? attr->defaultValue->resolved : std::string();
    if (pv.asset.empty() || resolved.empty()) {
        return x; // explicitly no texture
    }
    const std::string relPath = rel(resolved);
    x["path"] = str(relPath);
    if (!lower(relPath).ends_with(".dds")) {
        warn("unsupported_texture_format", relPath, "only DDS textures are loaded (as upstream)");
        x["status"] = str("unsupported");
        return x;
    }
    std::optional<std::vector<std::uint8_t>> bytes;
    std::string source = "file";
    if (auto t = files_(resolved)) {
        bytes = bytesOf(*t);
    } else {
        bytes = packagedTexture(relPath, &source);
    }
    if (!bytes) {
        warn("missing_texture", relPath, "texture not found (file or package) for " + param);
        x["status"] = str("missing");
        return x;
    }
    std::string err;
    const auto dds = assets::readDds(*bytes, &err);
    if (!dds) {
        warn("bad_texture", relPath, "DDS does not read: " + err);
        x["status"] = str("unreadable");
        return x;
    }
    const assets::TexFormat viewFormat =
        assets::resolveColourSpace(dds->image.format, assets::colourSpaceFromUsd(pv.colorSpace), param);
    const assets::TexFormatInfo* info = assets::texFormatInfo(dds->image.format);
    const assets::TexFormatInfo* viewInfo = assets::texFormatInfo(viewFormat);
    x["status"] = str("ok");
    x["source"] = str(source);
    x["blob"] = blob(*bytes, "image/vnd-ms.dds");
    x["format"] = str(info ? info->name : "unknown");
    x["width"] = num(dds->image.width);
    x["height"] = num(dds->image.height);
    x["mips"] = num(dds->image.mipLevels);
    x["srgb"] = Value::boolean(viewInfo && viewInfo->srgb);
    ++res_.counts.textures;
    if (ddsBytes) {
        *ddsBytes = std::move(*bytes);
    }
    return x;
}

const usd::Prim* findShader(const usd::ComposedStage& stage, const usd::Prim& material) {
    if (const usd::Prim* s = stage.find(material.path + "/Shader"); s && s->active && s->typeName == "Shader") {
        return s;
    }
    for (const std::string& c : material.children) {
        const usd::Prim* s = stage.find(material.path + "/" + c);
        if (s && s->active && s->typeName == "Shader") {
            return s;
        }
    }
    return nullptr;
}

std::string Importer::importMaterial(const std::string& path, Hash64 remixHash) {
    if (auto it = materialIds_.find(path); it != materialIds_.end()) {
        return it->second;
    }
    const usd::Prim* mat = stage_.find(path);
    if (!mat) {
        warn("missing_material", path, "bound material prim does not exist");
        materialIds_[path] = "";
        return "";
    }
    const bool looks = remixHash != 0;
    const std::string id = prefix_ + "/" + (looks ? hash::primName(hash::prim_prefix::kMaterial, remixHash) : idFromPath(path, ""));
    materialIds_[path] = id;
    ++res_.counts.materials;

    const usd::Prim* shader = findShader(stage_, *mat);
    std::string sourceAsset, subId;
    bool legacyPortal = false;
    if (shader) {
        if (const usd::Attribute* a = shader->attribute("info:mdl:sourceAsset"); a && a->hasDefault) {
            sourceAsset = a->defaultValue->text;
        }
        if (const usd::Attribute* a = shader->attribute("info:mdl:sourceAsset:subIdentifier"); a && a->hasDefault) {
            subId = a->defaultValue->text;
        }
        legacyPortal = shader->attribute("rayPortalIndex") != nullptr;
    } else {
        warn("no_shader", path, "material has no Shader prim; Opaque defaults");
    }
    bool known = true;
    const SurfaceType surface = surfaceTypeFromMdl(sourceAsset, legacyPortal, &known);
    if (!known) {
        warn("unknown_mdl", path, "unknown MDL '" + sourceAsset + "'; read as AperturePBR_Opacity (as upstream)");
    }
    std::vector<ParamIssue> issues;
    MaterialParams params = readMaterialParams(shader, surface, capture_, &issues);
    params.mdlSourceAsset = sourceAsset;
    params.mdlSubIdentifier = subId;
    for (const ParamIssue& i : issues) {
        warn("material_param", shader ? shader->path : path, i.param + ": " + i.message);
    }

    // Textures.
    TextureExtras extras;
    Value textureSet = Value::object();
    std::vector<std::uint8_t> albedoDds;
    bool albedoSrgb = true, anySlot = false;
    for (const ParamDesc& d : materialParamTable(surface)) {
        const std::string name(d.name);
        if (d.type != ParamType::Texture || !params.authored.count(name) || !shader) {
            continue;
        }
        const usd::Attribute* attr = shader->attribute("inputs:" + name);
        if (!attr && capture_) {
            attr = shader->attribute(name);
        }
        std::vector<std::uint8_t> dds;
        Value x = importTexture(params, name, attr, &dds);
        const std::string_view slot = textureSetSlot(surface, name);
        if (!slot.empty() && x.get("blob")) {
            textureSet[slot] = *x.get("blob");
            anySlot = true;
            if (slot == "albedo") {
                albedoSrgb = x.flag("srgb", true);
                albedoDds = dds;
            }
        }
        extras[name] = std::move(x);
    }
    const std::string texOaid = looks ? ex::originalAssetId(game_, ex::key_algo::kTexture, hash::hashToString(remixHash)) : "";
    if (anySlot) {
        Value r = header("texture_set", id, mat->name, path, "", {});
        Value& p = r["payload"];
        for (const auto& [k, v] : textureSet.o) {
            p[k] = v;
        }
        p["normalConvention"] = str("GL");
        p["texelsPerMetre"] = num(0);
        p["albedoSrgb"] = Value::boolean(albedoSrgb);
        if (capture_ && looks && !albedoDds.empty()) {
            Value rl = Value::object();
            rl["remix_hash"] = str(hash::hashToString(remixHash));
            rl["obsolete_hash"] = Value::boolean(false);
            p["relight"] = std::move(rl);
        }
        emit("texture_set", id, r);
    }
    // Keys: mat_<H> is the stage-0 texture hash (plan §4.1.4).
    if (looks) {
        const std::string hs = hash::hashToString(remixHash);
        originalAsset("texture", ex::key_algo::kTexture, hs, "");
        if (capture_ && !albedoDds.empty()) {
            if (const auto img = ex::readDds(albedoDds)) {
                if (const auto rgba = ex::decodeRgba8(img->format, img->width, img->height, img->mips.front())) {
                    captureTextureShas_.emplace_back(remixHash, ex::sha256Hex(rgba->data(), rgba->size()));
                }
            }
        }
        if (!capture_) {
            replacements_.push_back({texOaid, "default", "remix", id, prefix_, "material"});
        }
    }
    Value r = header("material", id, mat->name, path, "", looks && !capture_ ? std::vector<std::string>{texOaid} : std::vector<std::string>{});
    if (capture_ && looks) {
        r["provenance"]["derived_from"].push(str("oaid:" + texOaid));
    }
    r["payload"] = materialPayload(params, anySlot ? id : "");
    emit("material", id, r);
    Value e = header("material_ext", id, mat->name, path, "", {});
    e["payload"] = materialExtPayload(params, extras);
    e["payload"]["remix_hash"] = looks ? str(hash::hashToString(remixHash)) : Value();
    e["payload"]["particles"] = Value();
    if (auto ps = usd::readParticleSystem(*mat)) {
        e["payload"]["particles"] = str("poco:" + importParticles(id, *mat, *ps));
    }
    emit("material_ext", id, e);
    return id;
}

std::string Importer::importParticles(const std::string& ownerId, const usd::Prim& prim, const usd::ParticleSystem& ps) {
    Value r = header("relight_particles", ownerId, prim.name, prim.path, "", {});
    Value& p = r["payload"];
    p["owner"] = str("poco:" + ownerId);
    Value pv = Value::object();
    for (const auto& [name, v] : ps.primvars) {
        const auto parsed = json::parse(usd::valueToJson(v, modDir_));
        pv[name] = parsed ? *parsed : Value();
    }
    p["primvars"] = std::move(pv);
    emit("relight_particles", ownerId, r);
    ++res_.counts.particles;
    return ownerId;
}

std::string Importer::importLight(const usd::Prim& prim, const std::string& id, const Mat4d& transform, Hash64 remixHash,
                                  const std::string& attachedTo) {
    std::vector<std::string> issues;
    const LightParams params = readLightParams(prim, &issues);
    for (const std::string& i : issues) {
        warn("light_param", prim.path, i);
    }
    if (!isRemixLightType(prim.typeName)) {
        warn("unsupported_light", prim.path, prim.typeName + " is not a Remix light type");
    }
    std::string oaid;
    if (remixHash != 0) {
        oaid = originalAsset("light", ex::key_algo::kLight, hash::hashToString(remixHash), "");
        if (!capture_) {
            replacements_.push_back({oaid, "default", "remix", id, prefix_, "light"});
        }
    }
    Value r = header("light", id, prim.name, prim.path, capture_ ? oaid : "", !capture_ && !oaid.empty() ? std::vector<std::string>{oaid} : std::vector<std::string>{});
    r["payload"] = lightPayload(params);
    Value& rl = r["payload"]["relight"];
    rl["transform"] = matrixJson(transform);
    rl["remix_hash"] = remixHash ? str(hash::hashToString(remixHash)) : Value();
    rl["attached_to"] = attachedTo.empty() ? Value() : str("poco:" + attachedTo);
    emit("light", id, r);
    ++res_.counts.lights;
    return id;
}

Value Importer::categoriesJson(const usd::CategoryOverrides& c) const {
    Value o = Value::object();
    Value set = Value::array(), cleared = Value::array();
    for (std::uint32_t i = 0; i < scene::kInstanceCategoryCount; ++i) {
        const auto cat = static_cast<scene::InstanceCategories>(i);
        if (!c.exists.test(cat)) {
            continue;
        }
        (c.flags.test(cat) ? set : cleared).push(str(scene::instanceCategoryName(cat)));
    }
    o["set"] = std::move(set);
    o["cleared"] = std::move(cleared);
    return o;
}

void Importer::importMeshReplacement(const usd::MeshReplacement& r) {
    const std::string hs = hash::hashToString(r.hash);
    const std::string rootName = hash::primName(hash::prim_prefix::kMesh, r.hash);
    const std::string replId = prefix_ + "/" + rootName;
    const std::string oaid = originalAsset("mesh", geomAlgo_, hs, ruleId_);
    ++res_.counts.meshReplacements;

    Value parts = Value::array();
    bool firstMesh = true;
    for (const std::string& meshPath : r.meshPrims) {
        const usd::Prim* prim = stage_.find(meshPath);
        if (!prim) {
            continue;
        }
        std::vector<MeshIssue> issues;
        const auto mesh = importMesh(stage_, *prim, r.path, &issues);
        for (const MeshIssue& i : issues) {
            warn(i.error ? "mesh_rejected" : "mesh", meshPath, i.message);
        }
        if (!mesh) {
            continue;
        }
        std::vector<TransformIssue> tissues;
        const Mat4d xf = relativeTransform(stage_, meshPath, r.path, &tissues);
        for (const TransformIssue& t : tissues) {
            warn("xform", meshPath, t.message);
        }
        const std::string meshId = prefix_ + "/" + idFromPath(meshPath, std::string(usd::kMeshSection));
        // Materials of the submeshes.
        Value submeshes = Value::array();
        Value partMaterials = Value::array();
        for (const ImportedSubmesh& sm : mesh->submeshes) {
            std::string matId;
            if (!sm.materialPath.empty()) {
                const usd::Classification c = usd::classifyPrimPath(sm.materialPath);
                matId = importMaterial(sm.materialPath, c.cls == usd::PrimClass::Material ? c.hash : 0);
            }
            Value s = Value::object();
            s["indexOffset"] = num(sm.indexOffset);
            s["indexCount"] = num(sm.indexCount);
            s["materialSlot"] = str(matId.empty() ? "" : "poco:" + matId);
            s["subset"] = str(sm.subsetPath);
            submeshes.push(std::move(s));
            partMaterials.push(str(matId.empty() ? "" : "poco:" + matId));
        }
        Value rec = header("mesh", meshId, prim->name, meshPath, capture_ && firstMesh ? oaid : "", {});
        Value& p = rec["payload"];
        Value streams = Value::array();
        auto stream = [&](const char* semantic, const char* format, const std::vector<std::uint8_t>& bytes) {
            Value s = Value::object();
            s["semantic"] = str(semantic);
            s["format"] = str(format);
            s["data"] = blob(bytes, "fuse/mesh-stream");
            streams.push(std::move(s));
        };
        stream("Position", "F32x3", positionStream(*mesh));
        if (!mesh->normals.empty()) {
            stream("Normal", "F32x3", normalStream(*mesh));
        }
        if (!mesh->uv0.empty()) {
            stream("Uv0", "F32x2", uv0Stream(*mesh));
        }
        if (!mesh->colors.empty()) {
            stream("Color0", "Unorm8x4", color0Stream(*mesh));
        }
        if (mesh->influences > 4) {
            warn("mesh", meshPath, "more than 4 skinning influences per vertex; skinning streams dropped");
        } else if (mesh->influences > 0) {
            stream("Joints0", "U16x4", joints0Stream(*mesh));
            stream("Weights0", "F32x4", weights0Stream(*mesh));
        }
        p["streams"] = std::move(streams);
        p["indices32"] = blob(indices32Stream(*mesh), "fuse/mesh-stream");
        p["submeshes"] = std::move(submeshes);
        Value bounds = Value::array();
        for (float b : mesh->bounds) {
            bounds.push(num(double(b)));
        }
        p["bounds"] = std::move(bounds);
        p["skeletonId"] = str("");
        p["lodIds"] = Value::array();
        Value rl = Value::object();
        rl["remix_hash"] = str(hs);
        rl["double_sided"] = Value::boolean(mesh->doubleSided);
        rl["left_handed"] = Value::boolean(mesh->leftHanded);
        rl["face_corner_vertices"] = Value::boolean(mesh->expanded);
        rl["uv_primvar"] = str(mesh->uvPrimvar);
        rl["influences"] = num(mesh->influences);
        if (const usd::Relationship* skel = prim->relationship("skel:skeleton"); skel && !skel->targets.empty()) {
            rl["skeleton_prim"] = str(skel->targets.front());
        }
        p["relight"] = std::move(rl);
        emit("mesh", meshId, rec);
        ++res_.counts.meshes;
        if (capture_ && firstMesh) {
            ex::CaptureMesh cm;
            cm.points.assign(mesh->points.begin(), mesh->points.end());
            cm.indices.assign(mesh->indices.begin(), mesh->indices.end());
            captureMeshShas_.emplace_back(r.hash, ex::canonicalMeshSha256(cm));
        }
        firstMesh = false;
        Value part = Value::object();
        part["mesh"] = str("poco:" + meshId);
        part["prim"] = str(meshPath);
        part["transform"] = matrixJson(xf);
        part["materials"] = std::move(partMaterials);
        parts.push(std::move(part));
    }
    // Attached lights (relative to the replacement root, as upstream processLight on a mesh replacement).
    Value lights = Value::array();
    for (const std::string& lp : r.lightPrims) {
        const usd::Prim* prim = stage_.find(lp);
        if (!prim) {
            continue;
        }
        std::vector<TransformIssue> tissues;
        const Mat4d xf = relativeTransform(stage_, lp, r.path, &tissues);
        for (const TransformIssue& t : tissues) {
            warn("xform", lp, t.message);
        }
        const std::string lid = prefix_ + "/" + idFromPath(lp, std::string(usd::kMeshSection));
        lights.push(str("poco:" + importLight(*prim, lid, xf, 0, capture_ ? "" : replId)));
    }
    if (capture_) {
        return; // a captured mesh_<H> is the original asset, not a replacement
    }
    const usd::Prim* root = stage_.find(r.path);
    Value rec = header("relight_replacement", replId, rootName, r.path, "", {oaid});
    Value& p = rec["payload"];
    p["target_kind"] = str("mesh");
    Value key = Value::object();
    key["algo"] = str(geomAlgo_);
    key["value"] = str(hs);
    key["rule_id"] = str(ruleId_);
    key["rule"] = str(ruleString_);
    p["key"] = std::move(key);
    p["preserveOriginalDrawCall"] = r.preserveOriginalDrawCall ? Value::boolean(*r.preserveOriginalDrawCall) : Value();
    p["instanceable"] = Value::boolean(r.instanceable);
    p["categories"] = categoriesJson(r.categories);
    p["parts"] = std::move(parts);
    p["lights"] = std::move(lights);
    Value bindings = Value::array();
    for (const std::string& b : r.materialBindings) {
        bindings.push(str(b));
    }
    p["material_bindings"] = std::move(bindings);
    p["particles"] = Value();
    if (r.particles && root) {
        p["particles"] = str("poco:" + importParticles(replId, *root, *r.particles));
    }
    emit("relight_replacement", replId, rec);
    replacements_.push_back({oaid, "default", "remix", replId, prefix_, "relight_replacement"});
}

Value Importer::buildDb() {
    Value db = Value::object();
    db["schema"] = str(ex::kDbSchema);
    db["game_id"] = str(game_);
    std::sort(assets_.begin(), assets_.end(), [](const AssetRow& a, const AssetRow& b) { return a.oaid < b.oaid; });
    assets_.erase(std::unique(assets_.begin(), assets_.end(), [](const AssetRow& a, const AssetRow& b) { return a.oaid == b.oaid; }),
                  assets_.end());
    Value oa = Value::array();
    for (const AssetRow& a : assets_) {
        Value row = Value::object();
        row["oaid"] = str(a.oaid);
        row["kind"] = str(a.kind);
        row["game_id"] = str(game_);
        row["first_key"] = str(a.firstKey);
        oa.push(std::move(row));
    }
    db["original_asset"] = std::move(oa);

    if (capture_) {
        // RL-1.8 writePocoStore: canonical keys owned by the first asset (meshes, then textures, by hash) with that
        // content; the key list sorted and deduplicated by (algo, value).
        std::sort(captureMeshShas_.begin(), captureMeshShas_.end());
        std::sort(captureTextureShas_.begin(), captureTextureShas_.end());
        std::map<std::string, std::string> owner;
        for (const auto& [h, sha] : captureMeshShas_) {
            owner.try_emplace(sha, ex::originalAssetId(game_, geomAlgo_, hash::hashToString(h)));
            captureKeys_.push_back({ex::key_algo::kCaptureSha256, sha, "", "mesh"});
        }
        for (const auto& [h, sha] : captureTextureShas_) {
            owner.try_emplace(sha, ex::originalAssetId(game_, ex::key_algo::kTexture, hash::hashToString(h)));
            captureKeys_.push_back({ex::key_algo::kCaptureSha256, sha, "", "texture"});
        }
        std::sort(captureKeys_.begin(), captureKeys_.end());
        captureKeys_.erase(std::unique(captureKeys_.begin(), captureKeys_.end(),
                                       [](const ex::CaptureKey& a, const ex::CaptureKey& b) { return a.algo == b.algo && a.value == b.value; }),
                           captureKeys_.end());
        keys_.clear();
        for (const ex::CaptureKey& k : captureKeys_) {
            const std::string oaid = k.algo == ex::key_algo::kCaptureSha256 ? owner[k.value] : ex::originalAssetId(game_, k.algo, k.value);
            keys_.push_back({k.algo, k.value, oaid, k.ruleId, k.kind});
        }
    } else {
        std::sort(keys_.begin(), keys_.end(), [](const KeyRow& a, const KeyRow& b) {
            return std::tie(a.algo, a.value, a.ruleId, a.kind) < std::tie(b.algo, b.value, b.ruleId, b.kind);
        });
        keys_.erase(std::unique(keys_.begin(), keys_.end(), [](const KeyRow& a, const KeyRow& b) { return a.algo == b.algo && a.value == b.value; }),
                    keys_.end());
    }
    Value hk = Value::array();
    for (const KeyRow& k : keys_) {
        Value row = Value::object();
        row["algo"] = str(k.algo);
        row["value"] = str(k.value);
        row["oaid"] = str(k.oaid);
        row["rule_id"] = k.ruleId.empty() ? Value() : str(k.ruleId);
        row["kind"] = str(k.kind);
        hk.push(std::move(row));
    }
    res_.counts.keys = keys_.size();
    db["hash_key"] = std::move(hk);

    std::sort(replacements_.begin(), replacements_.end(), [](const ReplacementRow& a, const ReplacementRow& b) {
        return std::tie(a.oaid, a.variant, a.tier, a.source, a.pocoId) < std::tie(b.oaid, b.variant, b.tier, b.source, b.pocoId);
    });
    Value rp = Value::array();
    for (const ReplacementRow& r : replacements_) {
        Value row = Value::object();
        row["oaid"] = str(r.oaid);
        row["variant"] = str(r.variant);
        row["tier"] = str(r.tier);
        row["poco_id"] = str(r.pocoId);
        row["kind"] = str(r.recordKind);
        row["source"] = str(r.source);
        rp.push(std::move(row));
    }
    res_.counts.replacements = replacements_.size();
    db["replacement"] = std::move(rp);
    return db;
}

void Importer::run() {
    files_ = opt_.files ? opt_.files : usd::diskFileSource();
    if (opt_.gameId.empty()) {
        error("no_game", ".", "a game id is required (--game)");
        return;
    }
    if (!resolveStage()) {
        return;
    }
    game_ = sanitizeGame(opt_.gameId);
    usd::ReadOptions ro;
    ro.files = files_;
    stage_ = usd::readStage(stagePath_, ro);
    if (!stage_.ok) {
        error("no_root_layer", rel(stagePath_), "the root layer cannot be read");
        for (const usd::Diagnostic& d : stage_.diagnostics) {
            error(d.code, d.primPath.empty() ? rel(d.layer) : d.primPath, d.message);
        }
        return;
    }
    std::string layerType;
    if (const usd::Value* v = stage_.customLayerData.get("lightspeed_layer_type")) {
        layerType = v->asString().value_or("");
    }
    capture_ = opt_.kind == ImportKind::Capture || (opt_.kind == ImportKind::Auto && layerType == "capture");
    res_.capture = capture_;
    name_ = opt_.modName.empty() ? baseName(modDir_ == "." ? std::string("mod") : modDir_) : opt_.modName;
    if (name_.empty() || name_ == "." || name_ == "..") {
        name_ = "mod";
    }
    prefix_ = std::string(capture_ ? "capture/" : "mod/") + game_ + "/" + sanitizeSegment(name_);
    res_.idPrefix = prefix_;

    units_ = Value::object();
    units_["length"] = str("game");
    std::optional<double> mpu;
    std::string up = "+Y";
    for (const auto& [k, v] : stage_.layerMetadata) {
        if (k == "metersPerUnit") {
            mpu = v.asNumber();
        } else if (k == "upAxis" && v.asString().value_or("") == "Z") {
            up = "+Z";
        }
    }
    units_["metersPerUnit"] = mpu ? num(*mpu) : Value();
    units_["up"] = str(up);
    units_["handedness"] = str("right");

    for (const usd::Diagnostic& d : stage_.diagnostics) {
        diag(d.severity == usd::Diagnostic::Severity::Error ? "error" : "warning", d.code,
             d.primPath.empty() ? rel(d.layer) : d.primPath, d.message);
    }
    Value modPayload = Value::object();
    modPayload["name"] = str(name_);
    modPayload["kind"] = str(capture_ ? "capture" : "mod");
    modPayload["root_layer"] = str(rel(stagePath_));
    Value layers = Value::array();
    for (const std::string& l : stage_.layers) {
        layers.push(str(rel(l)));
    }
    modPayload["layers"] = std::move(layers);
    modPayload["layer_type"] = str(layerType);
    std::string gameName;
    if (const usd::Value* v = stage_.customLayerData.get("lightspeed_game_name")) {
        gameName = v->asString().value_or("");
    }
    modPayload["game_name"] = str(gameName);
    readModConfig(modPayload);
    detectLicence(modPayload);

    remix_ = usd::collectRemixMod(stage_);
    for (const usd::Diagnostic& d : remix_.diagnostics) {
        diag(d.severity == usd::Diagnostic::Severity::Error ? "error" : "warning", d.code, d.primPath, d.message);
    }
    // Upstream walk order: materials, meshes, lights.
    for (const usd::MaterialReplacement& m : remix_.materials) {
        importMaterial(m.path, m.hashFromName ? m.hash : 0);
    }
    for (const usd::MeshReplacement& r : remix_.meshes) {
        importMeshReplacement(r);
    }
    for (const usd::LightReplacement& l : remix_.lights) {
        const usd::Prim* prim = stage_.find(l.path);
        if (!prim) {
            continue;
        }
        if (!isUsdLightType(prim->typeName)) {
            warn("not_a_light", l.path, "light replacement prim has type '" + prim->typeName + "'; skipped");
            continue;
        }
        std::vector<TransformIssue> tissues;
        const Mat4d xf = relativeTransform(stage_, l.path, "", &tissues);
        for (const TransformIssue& t : tissues) {
            warn("xform", l.path, t.message);
        }
        importLight(*prim, prefix_ + "/" + idFromPath(l.path, std::string(usd::kLightsSection)), xf, l.hash, "");
    }
    // OmniGraph prims belong to the logic runtime (RL-3.5): listed, not imported.
    Value graphs = Value::array();
    for (const auto& [path, prim] : stage_.prims) {
        if (prim.typeName.compare(0, 9, "OmniGraph") == 0 && prim.typeName != "OmniGraphNode") {
            graphs.push(str(path));
        }
    }
    modPayload["graphs"] = std::move(graphs);

    res_.db = buildDb();
    res_.licenceId = licenceId_;
    Value counts = Value::object();
    counts["mesh_replacements"] = num(double(res_.counts.meshReplacements));
    counts["meshes"] = num(double(res_.counts.meshes));
    counts["materials"] = num(double(res_.counts.materials));
    counts["textures"] = num(double(res_.counts.textures));
    counts["lights"] = num(double(res_.counts.lights));
    counts["particles"] = num(double(res_.counts.particles));
    counts["keys"] = num(double(res_.counts.keys));
    counts["replacements"] = num(double(res_.counts.replacements));
    modPayload["counts"] = std::move(counts);
    Value diags = Value::array();
    for (const ImportDiagnostic& d : res_.diagnostics) {
        Value o = Value::object();
        o["severity"] = str(d.severity);
        o["code"] = str(d.code);
        o["where"] = str(d.where);
        o["message"] = str(d.message);
        diags.push(std::move(o));
    }
    modPayload["diagnostics"] = std::move(diags);
    Value mod = header("relight_mod", prefix_, name_, "", "", {});
    mod["payload"] = std::move(modPayload);
    emit("relight_mod", prefix_, mod);
    res_.counts.blobs = blobShas_.size();
    res_.files["db/remaster_db.json"] = bytesOf(json::writePretty(res_.db));
    res_.ok = true;
}

// ---- DB merge ---------------------------------------------------------------------------------------------------

std::string rowKey(const std::string& table, const Value& row) {
    if (table == "original_asset") {
        return row.str("oaid");
    }
    if (table == "hash_key") {
        return row.str("algo") + '\n' + row.str("value");
    }
    return row.str("oaid") + '\n' + row.str("variant") + '\n' + row.str("tier") + '\n' + row.str("source") + '\n' + row.str("poco_id");
}

} // namespace

std::size_t ImportResult::errors() const {
    return static_cast<std::size_t>(
        std::count_if(diagnostics.begin(), diagnostics.end(), [](const ImportDiagnostic& d) { return d.severity == "error"; }));
}

ImportResult importMod(const ImportOptions& options) {
    ImportResult res;
    Importer(options, res).run();
    return res;
}

Value mergeDb(const Value& a, const Value& b) {
    Value out = Value::object();
    out["schema"] = str(ex::kDbSchema);
    out["game_id"] = str(b.str("game_id", a.str("game_id")));
    for (const char* table : {"original_asset", "hash_key", "replacement"}) {
        std::map<std::string, Value> rows;
        for (const Value* doc : {&a, &b}) {
            if (const Value* t = doc->get(table); t && t->isArray()) {
                for (const Value& row : t->a) {
                    rows[rowKey(table, row)] = row; // b (inserted last) wins
                }
            }
        }
        Value arr = Value::array();
        for (auto& [k, row] : rows) {
            arr.push(std::move(row));
        }
        out[table] = std::move(arr);
    }
    return out;
}

bool writeStore(const fs::path& storeDir, const ImportResult& result, bool merge, std::string* error) {
    Value db = result.db;
    if (merge) {
        std::string existing;
        if (ex::readFile(storeDir / "db" / "remaster_db.json", existing)) {
            std::string perr;
            const auto old = json::parse(existing, &perr);
            if (!old || old->str("schema") != ex::kDbSchema) {
                if (error) {
                    *error = "existing db/remaster_db.json is not a " + std::string(ex::kDbSchema) + " document " + perr;
                }
                return false;
            }
            if (old->str("game_id") != result.db.str("game_id")) {
                if (error) {
                    *error = "existing store belongs to game '" + old->str("game_id") + "'";
                }
                return false;
            }
            db = mergeDb(*old, result.db);
        }
    }
    for (const auto& [rel, bytes] : result.files) {
        if (rel == "db/remaster_db.json") {
            continue;
        }
        if (!ex::writeFile(storeDir / fs::path(rel), bytes, error)) {
            return false;
        }
    }
    return ex::writeFile(storeDir / "db" / "remaster_db.json", json::writePretty(db), error);
}

VerifyResult verifyStore(const fs::path& storeDir) {
    VerifyResult res;
    auto err = [&](const std::string& e) { res.errors.push_back(e); };
    std::error_code ec;
    std::vector<fs::path> recordFiles;
    if (fs::is_directory(storeDir / "poco", ec)) {
        for (const auto& e : fs::recursive_directory_iterator(storeDir / "poco", ec)) {
            if (e.is_regular_file() && e.path().filename().string().ends_with(".poco.json")) {
                recordFiles.push_back(e.path());
            }
        }
    }
    std::sort(recordFiles.begin(), recordFiles.end());
    std::set<std::string> ids;                     ///< every record id (any kind)
    std::set<std::string> blobsChecked;
    std::vector<std::pair<std::string, std::string>> references; // (record, poco id)
    for (const fs::path& f : recordFiles) {
        const std::string rel = f.lexically_relative(storeDir).generic_string();
        std::string text;
        ex::readFile(f, text);
        std::string perr;
        const auto rec = json::parse(text, &perr);
        if (!rec || rec->str("schema") != ex::kPocoSchema) {
            err(rel + ": not a " + std::string(ex::kPocoSchema) + " record " + perr);
            continue;
        }
        ++res.records;
        const std::string kind = rec->str("kind"), id = rec->str("id");
        if (rel != recordPath(kind, id)) {
            err(rel + ": path does not match kind '" + kind + "' and id '" + id + "'");
        }
        ids.insert(id);
        for (const char* field : {"licence_id", "distribution"}) {
            if (rec->str(field).empty()) {
                err(rel + ": no " + std::string(field));
            }
        }
        std::vector<const Value*> stack{rec->get("payload")};
        while (!stack.empty()) {
            const Value* v = stack.back();
            stack.pop_back();
            if (!v) {
                continue;
            }
            if (isBlobRef(*v)) {
                const std::string sha = v->str("sha256");
                std::vector<std::uint8_t> bytes;
                if (sha.size() != 64 || !ex::readFile(storeDir / "blobs" / "sha256" / sha.substr(0, 2) / sha, bytes)) {
                    err(rel + ": missing blob " + sha);
                    continue;
                }
                if (bytes.size() != static_cast<std::size_t>(v->num("size", -1)) || ex::sha256Hex(bytes.data(), bytes.size()) != sha) {
                    err(rel + ": blob " + sha + " has the wrong size or content");
                }
                if (v->str("media") == "image/vnd-ms.dds" && blobsChecked.count(sha) == 0) {
                    std::string derr;
                    if (!assets::readDds(bytes, &derr)) {
                        err(rel + ": DDS blob " + sha + " does not read: " + derr);
                    }
                }
                blobsChecked.insert(sha);
            } else if (v->isObject()) {
                for (const auto& kv : v->o) {
                    stack.push_back(&kv.second);
                }
            } else if (v->isArray()) {
                for (const Value& e : v->a) {
                    stack.push_back(&e);
                }
            } else if (v->isString() && v->s.compare(0, 5, "poco:") == 0) {
                references.emplace_back(rel, v->s.substr(5));
            }
        }
    }
    res.blobs = blobsChecked.size();
    for (const auto& [rel, id] : references) {
        if (!ids.count(id)) {
            err(rel + ": references missing record poco:" + id);
        }
    }
    std::string dbText;
    if (!ex::readFile(storeDir / "db" / "remaster_db.json", dbText)) {
        err("missing db/remaster_db.json");
        return res;
    }
    const auto db = json::parse(dbText);
    if (!db || db->str("schema") != ex::kDbSchema) {
        err("db/remaster_db.json: not a " + std::string(ex::kDbSchema) + " document");
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
            if (colon == std::string::npos || ex::originalAssetId(game, first.substr(0, colon), first.substr(colon + 1)) != oaid) {
                err("original_asset " + oaid + ": does not recompute from its first key " + first);
            }
        }
    }
    std::set<std::pair<std::string, std::string>> seen;
    if (const Value* hk = db->get("hash_key")) {
        for (const Value& row : hk->a) {
            ++res.keys;
            const std::string algo = row.str("algo"), value = row.str("value");
            if (!seen.insert({algo, value}).second) {
                err("hash_key: duplicate (" + algo + ", " + value + ")");
            }
            const auto it = assetKind.find(row.str("oaid"));
            if (it == assetKind.end()) {
                err("hash_key (" + algo + ", " + value + "): unknown oaid '" + row.str("oaid") + "'");
            } else if (it->second != row.str("kind")) {
                err("hash_key (" + algo + ", " + value + "): kind " + row.str("kind") + " but the asset is a " + it->second);
            }
            if (algo.compare(0, 6, "remix.") == 0) {
                const auto parsed = hash::parseHashOption(value);
                if (value.size() != 16 || !parsed || hash::hashToString(*parsed) != value) {
                    err("hash_key (" + algo + ", " + value + "): not a 16-digit upper-case Remix hash");
                }
            }
        }
    }
    if (const Value* rp = db->get("replacement")) {
        std::set<std::string> rows;
        for (const Value& row : rp->a) {
            ++res.replacements;
            if (!rows.insert(rowKey("replacement", row)).second) {
                err("replacement: duplicate row for " + row.str("oaid"));
            }
            if (!assetKind.count(row.str("oaid"))) {
                err("replacement " + row.str("poco_id") + ": unknown oaid " + row.str("oaid"));
            }
            const std::string kind = row.str("kind");
            if (kind.empty() || !fs::exists(storeDir / recordPath(kind, row.str("poco_id")), ec)) {
                err("replacement: record " + kind + " '" + row.str("poco_id") + "' does not exist");
            }
        }
    }
    return res;
}

std::string detectSpdxLicence(const std::string& text) {
    const std::string marker = "SPDX-License-Identifier:";
    if (const std::size_t at = text.find(marker); at != std::string::npos) {
        std::size_t b = at + marker.size();
        while (b < text.size() && (text[b] == ' ' || text[b] == '\t')) {
            ++b;
        }
        std::size_t e = b;
        while (e < text.size() && text[e] != '\n' && text[e] != '\r' && text[e] != ' ' && text[e] != '\t' && text[e] != '*') {
            ++e;
        }
        if (e > b) {
            return text.substr(b, e - b);
        }
    }
    // Collapse whitespace so line-wrapped licence texts match.
    std::string t;
    bool space = false;
    for (const char c : text) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            space = !t.empty();
            continue;
        }
        if (space) {
            t += ' ';
            space = false;
        }
        t += c;
    }
    auto has = [&](std::string_view s) { return t.find(s) != std::string::npos; };
    if (has("CC0 1.0 Universal")) {
        return "CC0-1.0";
    }
    if (has("Attribution-NonCommercial-NoDerivatives 4.0 International")) {
        return "CC-BY-NC-ND-4.0";
    }
    if (has("Attribution-NonCommercial-ShareAlike 4.0 International")) {
        return "CC-BY-NC-SA-4.0";
    }
    if (has("Attribution-NonCommercial 4.0 International")) {
        return "CC-BY-NC-4.0";
    }
    if (has("Attribution-NoDerivatives 4.0 International")) {
        return "CC-BY-ND-4.0";
    }
    if (has("Attribution-ShareAlike 4.0 International")) {
        return "CC-BY-SA-4.0";
    }
    if (has("Attribution 4.0 International")) {
        return "CC-BY-4.0";
    }
    if (has("This is free and unencumbered software released into the public domain")) {
        return "Unlicense";
    }
    if (has("Apache License") && has("Version 2.0, January 2004")) {
        return "Apache-2.0";
    }
    if (has("Permission is hereby granted, free of charge, to any person obtaining a copy")) {
        return "MIT";
    }
    if (has("Redistribution and use in source and binary forms")) {
        return has("Neither the name") ? "BSD-3-Clause" : "BSD-2-Clause";
    }
    return {};
}

} // namespace fuse::relight::mods::import
