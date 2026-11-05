// FUSE Remaster W2.3: ingest-remix (see remix.hpp).
#include "project.hpp"

#include <fuse/remaster/digest.hpp>
#include <fuse/remaster/poco.hpp>
#include <fuse/remaster/remix/remix.hpp>

#include <fuse/relight/mods/import/mod_importer.hpp>
#include <fuse/relight/mods/usd/usd_stage.hpp>

#include <algorithm>
#include <map>
#include <set>

namespace fs = std::filesystem;
namespace rimp = fuse::relight::mods::import;
namespace rusd = fuse::relight::mods::usd;

namespace fuse::remaster::remix {
namespace {

bool isStageFile(const fs::path& p) {
    const std::string ext = p.extension().string();
    return ext == ".usda" || ext == ".usdc" || ext == ".usd";
}

/// The capture's root layer: a stage file, a folder with mod.usd*, or a folder with exactly one top-level stage.
std::string captureRoot(const fs::path& dir, std::string* error) {
    std::error_code ec;
    if (fs::is_regular_file(dir, ec)) {
        return dir.generic_string();
    }
    for (const char* name : {"mod.usda", "mod.usdc", "mod.usd"}) {
        if (fs::is_regular_file(dir / name, ec)) {
            return dir.generic_string(); // the Relight importer looks mod.usd* up itself
        }
    }
    std::vector<fs::path> stages;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        if (e.is_regular_file() && isStageFile(e.path())) {
            stages.push_back(e.path());
        }
    }
    if (stages.size() != 1) {
        *error = dir.string() + ": expected mod.usd* or exactly one top-level capture stage (found " +
                 std::to_string(stages.size()) + ")";
        return {};
    }
    return stages[0].generic_string();
}

std::string relightStr(const relight::capture::exporter::json::Value& v, const char* key) { return v.str(key); }

} // namespace

IngestReport ingestRemix(const IngestOptions& options) {
    IngestReport rep;
    std::string err;
    auto project = detail::loadProject(options.projectDir, &err);
    if (!project) {
        rep.errors.push_back(err);
        return rep;
    }
    const std::string root = captureRoot(options.captureDir, &err);
    if (root.empty()) {
        rep.errors.push_back(err);
        return rep;
    }

    // The capture's unit scale (the export writes replacement meshes back in these units).
    {
        const std::string stageFile = fs::is_directory(root) ? (fs::path(root) / "mod.usda").generic_string() : root;
        const rusd::ComposedStage stage = rusd::readStage(stageFile);
        for (const auto& [k, v] : stage.layerMetadata) {
            if (k == "metersPerUnit" && v.asNumber()) {
                rep.metersPerUnit = *v.asNumber();
            }
        }
    }

    rimp::ImportOptions io;
    io.root = root;
    io.gameId = project->gameId;
    io.assetRule = options.rule;
    io.kind = rimp::ImportKind::Capture;
    const rimp::ImportResult imported = rimp::importMod(io);
    for (const rimp::ImportDiagnostic& d : imported.diagnostics) {
        (d.severity == "error" ? rep.errors : rep.warnings).push_back(d.code + " " + d.where + ": " + d.message);
    }
    if (!imported.ok || !imported.capture) {
        rep.errors.push_back(imported.ok ? "not a capture (lightspeed_layer_type != \"capture\"): use a mod import"
                                         : "the capture could not be imported");
        return rep;
    }
    if (!rep.errors.empty()) {
        return rep;
    }

    // Records and blobs into the project store. Blobs are re-put through the BlobStore (atomic, address-checked);
    // records keep the importer's bytes and must read through fuse_poco.
    store::BlobStore blobs(project->store);
    for (const auto& [rel, bytes] : imported.files) {
        if (rel.starts_with("db/")) {
            continue;
        }
        if (rel.starts_with("blobs/sha256/")) {
            const auto ref = blobs.put(bytes, "application/octet-stream", &err);
            if (!ref || rel.substr(rel.rfind('/') + 1) != ref->sha256) {
                rep.errors.push_back(rel + ": blob does not store at its address (" + err + ")");
                continue;
            }
            ++rep.blobs;
        } else if (rel.starts_with("poco/")) {
            std::string perr;
            const auto rec = poco::fromJsonText(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), &perr);
            if (!rec) {
                rep.errors.push_back(rel + ": does not read as a POCO record: " + perr);
                continue;
            }
            if (rec->distribution != poco::Distribution::Never) {
                rep.errors.push_back(rel + ": a captured record must be distribution never");
            }
            if (!store::writeFileAtomic(project->store / rel, bytes, &err)) {
                rep.errors.push_back(err);
                continue;
            }
            ++rep.records;
        }
    }

    // DB rows: every original asset by its first key, then the other keys (remix64 <-> sha256 links).
    const auto* assets = imported.db.get("original_asset");
    const auto* keys = imported.db.get("hash_key");
    std::map<std::string, std::string> ruleOf; // "algo:value" -> rule_id
    if (keys) {
        for (const auto& k : keys->a) {
            ruleOf[relightStr(k, "algo") + ":" + relightStr(k, "value")] = relightStr(k, "rule_id");
        }
    }
    const std::size_t before = project->database->assets().size();
    std::set<std::string> oaids;
    if (assets) {
        for (const auto& a : assets->a) {
            const std::string first = relightStr(a, "first_key");
            const auto colon = first.find(':');
            if (colon == std::string::npos) {
                rep.errors.push_back("original asset with a malformed first_key '" + first + "'");
                continue;
            }
            const auto oaid = project->database->addOriginalAsset(project->gameId, relightStr(a, "kind"), first.substr(0, colon),
                                                                  first.substr(colon + 1), ruleOf[first], &err);
            if (!oaid || *oaid != relightStr(a, "oaid")) {
                rep.errors.push_back("original asset " + first + ": " + (oaid ? "oaid mismatch " + *oaid : err));
                continue;
            }
            oaids.insert(*oaid);
        }
    }
    if (keys) {
        for (const auto& k : keys->a) {
            if (!project->database->addHashKey(relightStr(k, "algo"), relightStr(k, "value"), relightStr(k, "oaid"),
                                               relightStr(k, "rule_id"), &err)) {
                rep.errors.push_back("hash key " + relightStr(k, "algo") + ":" + relightStr(k, "value") + ": " + err);
            }
        }
    }
    for (const std::string& e : project->database->verify()) {
        rep.errors.push_back("db: " + e);
    }
    if (!rep.errors.empty()) {
        return rep;
    }
    rep.newAssets = project->database->assets().size() - before;
    if (rep.metersPerUnit > 0.0) {
        project->doc["remix"]["meters_per_unit"] = rep.metersPerUnit;
        if (!store::writeFileAtomic(project->dir / "remaster.project.json", std::string_view(json::writePretty(project->doc)), &err)) {
            rep.errors.push_back(err);
        }
    }
    if (!detail::saveProjectDb(*project, &err)) {
        rep.errors.push_back(err);
        return rep;
    }
    for (const std::string& oaid : oaids) {
        const auto a = project->database->asset(oaid);
        IngestedAsset out;
        out.oaid = oaid;
        out.kind = a ? a->kind : "";
        out.firstKey = a ? a->firstKey : "";
        for (const db::HashKey& k : project->database->keysOf(oaid)) {
            out.keys.emplace_back(k.algo, k.value);
        }
        std::sort(out.keys.begin(), out.keys.end());
        rep.assets.push_back(std::move(out));
    }
    rep.ok = rep.errors.empty();
    return rep;
}

json::Value toJson(const IngestReport& r) {
    json::Value out = json::Value::object();
    out["schema"] = "fuse.remaster.remix-ingest/1";
    out["ok"] = r.ok;
    out["records"] = static_cast<std::uint64_t>(r.records);
    out["blobs"] = static_cast<std::uint64_t>(r.blobs);
    out["new_assets"] = static_cast<std::uint64_t>(r.newAssets);
    out["meters_per_unit"] = r.metersPerUnit;
    json::Value assets = json::Value::array();
    for (const IngestedAsset& a : r.assets) {
        json::Value o = json::Value::object();
        o["oaid"] = a.oaid;
        o["kind"] = a.kind;
        o["first_key"] = a.firstKey;
        json::Value ks = json::Value::object();
        for (const auto& [algo, value] : a.keys) {
            ks[algo] = value;
        }
        o["keys"] = std::move(ks);
        assets.push(std::move(o));
    }
    out["assets"] = std::move(assets);
    json::Value errs = json::Value::array(), warns = json::Value::array();
    for (const auto& e : r.errors) {
        errs.push(e);
    }
    for (const auto& w : r.warnings) {
        warns.push(w);
    }
    out["errors"] = std::move(errs);
    out["warnings"] = std::move(warns);
    return out;
}

} // namespace fuse::remaster::remix
