// FUSE Remaster W0.3: fuse_remaster_db, the replacement DB (docs/plans/FUSE_REMASTER_PLAN.md §1.3).
//
// `remaster.db` is SQLite (vendor/sqlite). Tables:
//   original_asset (oaid PK, kind, game_id, first_key)          no payload, ever (§1.3 rule 3)
//   hash_key       ((algo, value) PK -> oaid, rule_id)          several keys per original (sha256, remix64, ...)
//   replacement    ((oaid, variant, tier, source) PK -> poco_id, kind, fit)
//   recipe         (recipe_id PK, status draft|approved|rejected, recipe JSON)
//   review         ((recipe_id, date, reviewer) PK, decision, flip, ssim, notes)
// `remaster.lock.json` is the canonical export: every table, sorted by its key, written with sorted members,
// so exporting the same DB twice (or a DB rebuilt from its own lock) is byte-identical. The lock is what our
// projects commit and diff in review; the .db is rebuilt from it (saveAs writes rows in key order, so the .db
// bytes are deterministic too).
//
// oaid = UUIDv5(UUIDv5(NameSpace_URL, "https://fuse.invalid/remaster/oaid"), "<game>/<algo>:<value>") of the
// asset's first key, <game> being the sanitised game id ([a-z0-9_-], other characters '_'). This is the rule
// the Relight capture store (RL-1.8) has used since before W0.3, so its oaids are unchanged.
#pragma once

#include <fuse/remaster/json.hpp>

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fuse::remaster::db {

inline constexpr const char* kLockSchema = "fuse.remaster.lock/1";
inline constexpr int kDbSchemaVersion = 1;

/// Lower-case [a-z0-9_-], anything else '_'; "game" when empty.
std::string sanitizeGameId(std::string_view gameId);
/// The oaid of an original asset whose first key is (algo, value) in game `gameId`.
std::string originalAssetId(std::string_view gameId, std::string_view algo, std::string_view value);

struct OriginalAsset {
    std::string oaid, kind, gameId, firstKey; // firstKey = "<algo>:<value>"
    friend bool operator==(const OriginalAsset&, const OriginalAsset&) = default;
};
struct HashKey {
    std::string algo, value, oaid, ruleId;
    std::string kind; // the asset's kind (filled on read)
    friend bool operator==(const HashKey&, const HashKey&) = default;
};
struct Replacement {
    std::string oaid;
    std::string variant = "default"; // default | lowspec | stylised | ...
    std::string tier;                // T0..T3 | remix
    std::string source;              // the mod / package that provided it ("" for the project itself)
    std::string pocoId;
    std::string kind;
    json::Value fit; // §2.7 fit block (translate, rotate, scale, pivot); null = none
    friend bool operator==(const Replacement&, const Replacement&) = default;
};
struct Recipe {
    std::string recipeId;
    std::string status = "draft"; // draft | approved | rejected
    json::Value recipe;           // the fuse.ai-recipe/1 document (or null)
    friend bool operator==(const Recipe&, const Recipe&) = default;
};
struct Review {
    std::string recipeId, date, reviewer, decision, notes;
    std::optional<double> flip, ssim;
    friend bool operator==(const Review&, const Review&) = default;
};

class Database {
public:
    /// Opens (or creates) a DB file; ":memory:" for an in-memory DB. Creates the schema when missing and refuses
    /// a DB with a newer schema version.
    static std::unique_ptr<Database> open(const std::filesystem::path& path, std::string* error = nullptr);
    static std::unique_ptr<Database> openMemory(std::string* error = nullptr) { return open(":memory:", error); }
    ~Database();
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    /// Registers an original asset by its first key and returns its oaid. Idempotent: an existing asset with the
    /// same oaid and kind is fine. The key is also added as a hash_key row.
    std::optional<std::string> addOriginalAsset(std::string_view gameId, std::string_view kind, std::string_view algo,
                                                std::string_view value, std::string_view ruleId = {}, std::string* error = nullptr);
    /// Adds another key to an existing asset. Idempotent for the same (algo, value, oaid); a key already bound to a
    /// different oaid is an error (one key, one original).
    bool addHashKey(std::string_view algo, std::string_view value, std::string_view oaid, std::string_view ruleId = {},
                    std::string* error = nullptr);
    /// The oaid a key resolves to.
    std::optional<std::string> resolve(std::string_view algo, std::string_view value) const;
    std::optional<OriginalAsset> asset(std::string_view oaid) const;
    std::vector<HashKey> keysOf(std::string_view oaid) const;

    /// Inserts or replaces a replacement row (its oaid must exist).
    bool setReplacement(const Replacement& r, std::string* error = nullptr);
    bool removeReplacement(std::string_view oaid, std::string_view variant, std::string_view tier, std::string_view source);
    std::vector<Replacement> replacementsOf(std::string_view oaid) const;

    bool putRecipe(const Recipe& r, std::string* error = nullptr);
    bool addReview(const Review& r, std::string* error = nullptr);

    std::vector<OriginalAsset> assets() const;
    std::vector<HashKey> hashKeys() const;
    std::vector<Replacement> replacements() const;
    std::vector<Recipe> recipes() const;
    std::vector<Review> reviews() const;

    /// The canonical remaster.lock.json document / text.
    json::Value exportLock() const;
    std::string exportLockText() const;
    /// Merges a lock into this DB (conflicting rows are an error; everything or nothing is applied).
    bool importLock(const json::Value& lock, std::string* error = nullptr);
    /// Writes this DB to a new file (replacing it), rows in key order: deterministic bytes.
    bool saveAs(const std::filesystem::path& path, std::string* error = nullptr) const;

    /// Integrity: every oaid recomputes from its first key, every hash_key and replacement points at an asset,
    /// every asset's first key is one of its keys, review rows point at recipes. Empty = consistent.
    std::vector<std::string> verify() const;

private:
    struct Impl;
    explicit Database(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> m_impl;
};

/// Reads remaster.lock.json text into a fresh in-memory DB.
std::unique_ptr<Database> loadLock(std::string_view lockText, std::string* error = nullptr);

} // namespace fuse::remaster::db
