// FUSE Relight RL-3.2: the mod importer. A Remix mod (or a Relight / Remix capture: captures are mods too) becomes
// POCO records + replacement-DB rows in the RL-1.8 store shim layout (docs/plans/FUSE_REMIX_PORT_PLAN.md §4.4,
// §4.5, §4.8, §4.1.5; Remaster plan §1.3, §2.1-§2.3):
//
//   <store>/poco/<kind>/<id>.poco.json   records (schema fuse.poco/1)
//   <store>/blobs/sha256/<ab>/<sha256>   content-addressed payloads (mesh streams, DDS files, licence text, rtx.conf)
//   <store>/db/remaster_db.json          original_asset, hash_key, replacement (sorted; merged with an existing DB)
//
// Pipeline: usd::readStage (RL-3.1 Remix-profile composition) -> usd::collectRemixMod (mesh_ / mat_ / light_
// classification) -> records:
//
//   mod (lightspeed_layer_type != "capture")                      capture (lightspeed_layer_type == "capture")
//   ------------------------------------------------------------  -------------------------------------------------
//   relight_mod        one per import: layers, rtx.conf, licence,   same (origin "original")
//                      diagnostics, OmniGraph prim paths
//   relight_replacement one per mesh_<H>: key, preserveOriginal-   - (a captured mesh_<H> is the original itself)
//                      DrawCall, categories, parts (mesh + local
//                      transform + materials), attached lights,
//                      particles
//   mesh               one per UsdGeomMesh below a mesh_<H>        one per mesh_<H> (original_asset = its oaid)
//   material +         one per mat_<H> and per other bound         one per mat_<H>
//   material_ext +     material (the §4.5 table, material_table.hpp)
//   texture_set
//   light              light_<H> and lights below mesh_<H>         light_<H> (original_asset = its oaid)
//   relight_particles  ParticleSystemAPI prims (mesh / material)   same
//
// DB rows. Keys follow plan §4.1.5: a mesh_<H> is `remix.geom.asset` (or `remix.geom.legacy0/1` when the asset
// rule is a legacy rule) qualified by rule_id; a mat_<H> is `remix.tex` (the material hash is the stage-0 texture
// hash); a light_<H> is `remix.light`. Every key's oaid is capture::exporter::originalAssetId(game, algo, value),
// so a mod's replacement rows and a capture of the same game meet on the same original asset. Mods add
// replacement rows (oaid, variant "default", tier "remix") -> the relight_replacement / material / light record;
// captures add `fuse.capture.sha256` keys (canonical mesh stream, RGBA8 mip 0 of the DX9 DDS) exactly as RL-1.8's
// writePocoStore does, so importing an RL-1.8 capture reproduces its key set.
//
// Licence and provenance (plan §4.8): records of a mod are provenance.origin "derived" with derived_from
// ["mod:<name>"], licence_id = the SPDX id found in the mod's own licence file (SPDX-License-Identifier line, or a
// known licence text), else `LicenseRef-ThirdPartyMod-<name>`; distribution is always "never" (FUSE never ships a
// third-party mod: Remaster §0.3 non-goal). Captures are origin "original", `LicenseRef-Original-<game>`, "never",
// as RL-1.8.
//
// Determinism: records and the DB are pretty JSON with a fixed member order, rows sorted, every path relative to
// the mod directory, no time stamps and no absolute paths, so two imports of the same mod give identical bytes.
#pragma once

#include <fuse/relight/capture/export/json.hpp>
#include <fuse/relight/mods/usd/usd_stage.hpp>

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace fuse::relight::mods::import {

inline constexpr const char* kImportTool = "Source/FUSE/Relight/mods/import (RL-3.2 fuse_relight_import)";

enum class ImportKind : std::uint8_t { Auto, Mod, Capture };

struct ImportOptions {
    std::string root;      ///< mod directory (mod.usda / mod.usdc / mod.usd is looked up) or a stage file
    std::string gameId;    ///< Remaster game id (store namespace, oaid seed); required
    std::string modName;   ///< default: the mod directory's name
    std::string assetRule; ///< rtx.geometryAssetHashRuleString; default: --rule, else the mod's rtx.conf, else the
                           ///< capture's lightspeed_geometry_hash_rules, else the Remix default
    ImportKind kind = ImportKind::Auto;
    usd::FileSource files; ///< default: disk (layers, textures, packages, licence files, rtx.conf)
    /// `.pkg` / `.rtxio` packages of the mod directory for textures missing on disk (file names relative to the
    /// mod directory, searched in reverse alphabetical order as upstream). Default: listed from disk.
    std::optional<std::vector<std::string>> packages;
};

struct ImportDiagnostic {
    std::string severity; ///< "warning" | "error"
    std::string code;
    std::string where;    ///< prim path or mod-relative file
    std::string message;
};

struct ImportCounts {
    std::size_t meshReplacements = 0, meshes = 0, materials = 0, textures = 0, lights = 0, particles = 0, records = 0,
                blobs = 0, keys = 0, replacements = 0;
};

struct ImportResult {
    bool ok = false;           ///< false on a fatal problem (no root layer, missing game id)
    bool capture = false;      ///< imported as a capture
    std::string idPrefix;      ///< "mod/<game>/<name>" or "capture/<game>/<name>"
    std::string licenceId;
    /// Store-relative path -> bytes: every record, blob and db/remaster_db.json (the DB of this import alone).
    std::map<std::string, std::vector<std::uint8_t>> files;
    capture::exporter::json::Value db; ///< the DB tables of this import
    std::vector<ImportDiagnostic> diagnostics;
    ImportCounts counts;
    std::size_t errors() const;
};

/// Imports a mod or capture into memory.
ImportResult importMod(const ImportOptions& options);

/// Writes `result.files` under `storeDir`. With `merge`, an existing db/remaster_db.json of the same game is
/// merged (rows unioned by key, this import's rows win) before it is written. false on an I/O error.
bool writeStore(const std::filesystem::path& storeDir, const ImportResult& result, bool merge, std::string* error = nullptr);

/// Merges two DB documents (same game id): rows unioned by key, `b` wins, sorted.
capture::exporter::json::Value mergeDb(const capture::exporter::json::Value& a, const capture::exporter::json::Value& b);

/// Checks a store on disk: every record parses, lives at the path its kind and id give, references existing
/// records (poco:<id>) and blobs of the right size and sha256; DDS blobs read with the RL-3.3 reader; the DB's
/// keys are unique, point at original assets whose oaid recomputes from their first key, and every replacement
/// row names an existing record.
struct VerifyResult {
    std::vector<std::string> errors;
    std::size_t records = 0, blobs = 0, keys = 0, replacements = 0;
    bool ok() const { return errors.empty(); }
};
VerifyResult verifyStore(const std::filesystem::path& storeDir);

/// Licence detection (plan §4.8): the SPDX id of a licence text ("SPDX-License-Identifier:" line, else a known
/// licence's signature), "" when unknown.
std::string detectSpdxLicence(const std::string& text);

} // namespace fuse::relight::mods::import
