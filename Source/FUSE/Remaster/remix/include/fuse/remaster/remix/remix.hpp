// FUSE Remaster W2.3 / W2.4: Remix-format interop for a remaster project (docs/plans/FUSE_REMASTER_PLAN.md §2.5,
// §7 Wave 2; the formats are pinned in docs/remaster/remix-format-notes.md).
//
//   ingestRemix   a Remix (or FUSE Relight) capture -> POCO records + blobs in the project store, original_asset +
//                 hash_key rows in the project DB (remaster.lock.json + remaster.db). The capture is read by FUSE
//                 Relight's mod importer (RL-3.2: Remix-profile USD composition on vendor/tinyusdz); every Remix
//                 key (remix.geom.asset / remix.tex / remix.light) gets its oaid by the W0.3 rule, and meshes and
//                 textures with readable payloads are linked to their canonical sha256 (fuse.capture.sha256). The
//                 records are originals (LicenseRef-Original-<game>, distribution never); the store lives in the
//                 git-ignored per-game cache in real use (§0.1.2).
//   exportRemix   the project's replacement rows (tier "remix") -> a Remix mod folder:
//                   mod.usda                        replacement layer (lightspeed_layer_type "replacement"): an
//                                                   `over` per captured prim, mesh_<H> -> def Xform "ref_<name>"
//                                                   referencing our mesh layer; sublayer materials/materials.usda
//                   materials/materials.usda        def Material "mat_<H>" (AperturePBR_Opacity by name) with the
//                                                   texture inputs
//                   assets/meshes/<name>.usda       one UsdGeomMesh per replacement mesh (points in game units)
//                   assets/textures/<name>_<slot>.dds  BC7 (albedo, sRGB / ORM / emissive), BC5 (normal), DX10 header
//                   remaster-export.json            manifest (schema fuse.remaster.remix-export/1)
//                 Only shippable records are exported (a recipe-only derivative only with `local`, marked
//                 local-only in the manifest); originals never are.
//   checkExport   validation of a mod folder: the manifest schema, every DDS reads back (Relight's reader), the
//                 composed mod stage classifies every replacement prim with its hash and no error diagnostics,
//                 re-ingest of the mod (Relight importer, kind Mod) finds a replacement row for every exported
//                 oaid, and the lint: no file of the mod is (or references) a blob of an original / never /
//                 recipe-only record of the project store, and every USD asset path stays inside the mod folder.
#pragma once

#include <fuse/remaster/json.hpp>

#include <cstddef>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace fuse::remaster::remix {

inline constexpr const char* kExportSchema = "fuse.remaster.remix-export/1";
inline constexpr const char* kManifestFile = "remaster-export.json";

struct IngestOptions {
    std::filesystem::path captureDir; ///< folder with the capture stage (mod.usda / capture.usda / *.usda root)
    std::filesystem::path projectDir; ///< a `fuse_remaster init` project
    std::string rule;                 ///< geometry asset hash rule; default: the capture's own
};

struct IngestedAsset {
    std::string oaid, kind, firstKey;
    std::vector<std::pair<std::string, std::string>> keys; ///< (algo, value), sorted
};

struct IngestReport {
    bool ok = false;
    std::vector<std::string> errors, warnings;
    std::vector<IngestedAsset> assets; ///< every original asset of this capture, sorted by oaid
    std::size_t records = 0, blobs = 0, newAssets = 0;
    double metersPerUnit = 0.0; ///< the capture's stage metersPerUnit (0 when not authored)
};

IngestReport ingestRemix(const IngestOptions& options);
json::Value toJson(const IngestReport& report);

struct ExportOptions {
    std::filesystem::path projectDir;
    std::filesystem::path outDir;      ///< created; existing files with our names are overwritten
    std::string modName = "fuse_remaster";
    bool local = false;                ///< allow recipe-only (derivative) records: a local, never-shipped mod
};

struct ExportedReplacement {
    std::string oaid, kind; ///< kind: "mesh" | "material"
    std::string remixHash;  ///< 16 upper-case hex digits
    std::string prim;       ///< /RootNode/meshes/mesh_<H> or /RootNode/Looks/mat_<H>
    std::string pocoId, pocoKind;
    std::string meshLayer;  ///< mod-relative, meshes only
    std::map<std::string, std::string> textures; ///< slot -> mod-relative DDS path
};

struct ExportReport {
    bool ok = false;
    std::vector<std::string> errors;
    std::vector<ExportedReplacement> replacements; ///< sorted by prim
    std::vector<std::string> files;                ///< mod-relative, sorted
};

ExportReport exportRemix(const ExportOptions& options);

struct CheckReport {
    std::vector<std::string> errors;
    std::vector<std::string> foundOaids; ///< oaids with a replacement row in the re-ingested mod, sorted
    bool ok() const { return errors.empty(); }
};

/// Validates `modDir` (see the file comment). `projectDir` provides the game id and the store for the lint.
CheckReport checkExport(const std::filesystem::path& modDir, const std::filesystem::path& projectDir);

} // namespace fuse::remaster::remix
