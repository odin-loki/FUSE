// FUSE Remaster W2.3 / W2.4: the bits of a `fuse_remaster init` project the Remix commands need (internal).
#pragma once

#include <fuse/remaster/db.hpp>
#include <fuse/remaster/json.hpp>
#include <fuse/remaster/store.hpp>

#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace fuse::remaster::remix::detail {

struct Project {
    std::filesystem::path dir;
    json::Value doc;
    std::string gameId;
    std::filesystem::path store, lock, db;
    std::unique_ptr<db::Database> database;
};

inline std::optional<Project> loadProject(const std::filesystem::path& dir, std::string* error) {
    Project p;
    p.dir = dir;
    const auto text = store::readFileText(dir / "remaster.project.json");
    if (!text) {
        *error = "no remaster.project.json in " + dir.string();
        return std::nullopt;
    }
    auto doc = json::parse(*text);
    if (!doc || !doc->get("game_id") || doc->get("game_id")->asString().empty()) {
        *error = "remaster.project.json has no game_id";
        return std::nullopt;
    }
    p.doc = std::move(*doc);
    p.gameId = p.doc.get("game_id")->asString();
    auto member = [&](const char* key, const char* fallback) {
        const json::Value* v = p.doc.get(key);
        return dir / ((v && !v->asString().empty()) ? v->asString() : std::string(fallback));
    };
    p.store = member("store", "store");
    p.lock = member("lock", "remaster.lock.json");
    p.db = member("db", "remaster.db");
    const auto lock = store::readFileText(p.lock);
    p.database = lock ? db::loadLock(*lock, error) : db::Database::openMemory(error);
    if (!p.database) {
        return std::nullopt;
    }
    return p;
}

/// Writes the canonical lock and the deterministic .db next to it.
inline bool saveProjectDb(const Project& p, std::string* error) {
    return store::writeFileAtomic(p.lock, std::string_view(p.database->exportLockText()), error) &&
           p.database->saveAs(p.db, error);
}

} // namespace fuse::remaster::remix::detail
