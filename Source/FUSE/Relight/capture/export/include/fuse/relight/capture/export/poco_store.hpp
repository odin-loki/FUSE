// FUSE Relight RL-1.8: the FUSE capture as POCO records + hash_key rows (plan §4.1.5, §4.8; Remaster
// plan §1.3, §2.1-§2.3).
//
// The store uses the Remaster W0 libraries (docs/plans/FUSE_REMASTER_PLAN.md §1.3, §2.1): blobs go through
// fuse_remaster_store's BlobStore, the replacement DB through fuse_remaster_db, and every record reads back
// through fuse_poco on re-ingest. Layout:
//
//   <store>/poco/<kind>/<poco_id>.poco.json   header (schema fuse.poco/1, kind, id, name, units, payload,
//                                             provenance, licence_id, distribution, replaces, tags, review)
//   <store>/blobs/sha256/<ab>/<sha256>        content-addressed payloads
//   <store>/db/remaster.db                    the replacement DB (SQLite, fuse_remaster_db; deterministic bytes)
//   <store>/db/remaster.lock.json             its canonical export (fuse.remaster.lock/1)
//   <store>/db/remaster_db.json               the same original_asset (oaid, kind, game_id, first_key) and
//                                             hash_key (algo, value, oaid, rule_id, kind) rows as the JSON view
//                                             the Relight readers (mods/import, replace) consume; replacement
//                                             empty. Re-ingest checks the three agree.
//
// Records written per capture (poco_id "cap/<game>/<prim name>"): mesh (Position / Normal / Uv0 / Color0 /
// Joints0 / Weights0 streams, indices32, one submesh), texture_set (albedo = the DDS blob), material and its
// material_ext (the legacy blend / alpha-test / sampler state, plan §4.4 relight_material), light, and one
// level (instances as entities with their first transform, the light ids, the camera). Captured originals
// are provenance.origin "original", distribution "never", licence LicenseRef-Original-<game>: the DB never
// ships them (Remaster §0.1). Each original asset's `oaid` is the UUIDv5 of "<game id>/<algo>:<value>" of
// its first Remix key in the FUSE oaid namespace (UUIDv5(NameSpace_URL, "https://fuse.invalid/remaster/oaid")).
#pragma once

#include <fuse/relight/capture/export/capture_model.hpp>

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace fuse::relight::capture::exporter {

inline constexpr const char* kPocoSchema = "fuse.poco/1";
inline constexpr const char* kDbSchema = "fuse.remaster.db-shim/1"; ///< db/remaster_db.json (the reader view)
inline constexpr const char* kLockFile = "remaster.lock.json";     ///< under <store>/db/
inline constexpr const char* kSqliteFile = "remaster.db";          ///< under <store>/db/

/// The Remaster oaid of an original asset keyed by (algo, value) in game `gameId`.
std::string originalAssetId(const std::string& gameId, const std::string& algo, const std::string& value);

struct StoreReport {
    std::vector<std::string> records; ///< poco files written (store-relative)
    std::size_t blobs = 0;
    std::vector<CaptureKey> keys;     ///< the hash_key rows written
};

/// Writes the capture into `storeDir` (created; existing files are overwritten). `ddsFiles` holds the DDS
/// bytes written for each texture (textures without one get a texture_set without an albedo blob).
bool writePocoStore(const std::filesystem::path& storeDir, const CaptureData& capture, hash::HashRule assetRule,
                    const std::map<Hash64, std::vector<std::uint8_t>>& ddsFiles, StoreReport* report = nullptr,
                    std::string* error = nullptr);

/// Re-ingest of a store: parses the DB and every POCO record, checks every referenced blob (present, size,
/// sha256), every texture blob (a readable DDS whose mip 0 hashes back to the texture's Remix key) and the DB
/// (each key's oaid is an original asset; each asset's oaid recomputes from its first Remix key; keys unique).
struct IngestResult {
    std::vector<std::string> errors;
    std::vector<CaptureKey> keys; ///< hash_key rows as read, sorted
    std::size_t records = 0;
    std::size_t blobs = 0;
    std::size_t assets = 0;
    bool ok() const { return errors.empty(); }
};
IngestResult ingestPocoStore(const std::filesystem::path& storeDir);

/// Whole-file helpers (binary).
bool writeFile(const std::filesystem::path& path, const std::string& bytes, std::string* error = nullptr);
bool writeFile(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes, std::string* error = nullptr);
bool readFile(const std::filesystem::path& path, std::vector<std::uint8_t>& bytes);
bool readFile(const std::filesystem::path& path, std::string& text);

} // namespace fuse::relight::capture::exporter
