// FUSE Remaster W0.3: the replacement DB on SQLite (see db.hpp).
#include <fuse/remaster/db.hpp>
#include <fuse/remaster/digest.hpp>

#include <sqlite3.h>

#include <algorithm>
#include <set>
#include <tuple>

namespace fuse::remaster::db {

namespace fs = std::filesystem;

namespace {

const char* kSchemaSql = R"sql(
CREATE TABLE IF NOT EXISTS meta (key TEXT PRIMARY KEY, value TEXT NOT NULL) WITHOUT ROWID;
CREATE TABLE IF NOT EXISTS original_asset (
    oaid TEXT PRIMARY KEY, kind TEXT NOT NULL, game_id TEXT NOT NULL, first_key TEXT NOT NULL) WITHOUT ROWID;
CREATE TABLE IF NOT EXISTS hash_key (
    algo TEXT NOT NULL, value TEXT NOT NULL, oaid TEXT NOT NULL REFERENCES original_asset(oaid),
    rule_id TEXT NOT NULL DEFAULT '', PRIMARY KEY (algo, value)) WITHOUT ROWID;
CREATE INDEX IF NOT EXISTS hash_key_oaid ON hash_key(oaid);
CREATE TABLE IF NOT EXISTS replacement (
    oaid TEXT NOT NULL REFERENCES original_asset(oaid), variant TEXT NOT NULL, tier TEXT NOT NULL,
    source TEXT NOT NULL DEFAULT '', poco_id TEXT NOT NULL, kind TEXT NOT NULL DEFAULT '', fit TEXT NOT NULL DEFAULT 'null',
    PRIMARY KEY (oaid, variant, tier, source)) WITHOUT ROWID;
CREATE TABLE IF NOT EXISTS recipe (
    recipe_id TEXT PRIMARY KEY, status TEXT NOT NULL CHECK (status IN ('draft','approved','rejected')),
    recipe TEXT NOT NULL DEFAULT 'null') WITHOUT ROWID;
CREATE TABLE IF NOT EXISTS review (
    recipe_id TEXT NOT NULL REFERENCES recipe(recipe_id), date TEXT NOT NULL, reviewer TEXT NOT NULL,
    decision TEXT NOT NULL, flip REAL, ssim REAL, notes TEXT NOT NULL DEFAULT '',
    PRIMARY KEY (recipe_id, date, reviewer)) WITHOUT ROWID;
)sql";

bool setError(std::string* error, const std::string& msg) {
    if (error) {
        *error = msg;
    }
    return false;
}

const Uuid& oaidNamespace() {
    static const Uuid ns = uuidV5(uuidNamespaceUrl(), "https://fuse.invalid/remaster/oaid");
    return ns;
}

/// RAII prepared statement.
class Stmt {
public:
    Stmt(sqlite3* db, const char* sql) : m_db(db) { sqlite3_prepare_v2(db, sql, -1, &m_s, nullptr); }
    ~Stmt() { sqlite3_finalize(m_s); }
    Stmt(const Stmt&) = delete;
    Stmt& operator=(const Stmt&) = delete;
    bool ok() const { return m_s != nullptr; }
    Stmt& bind(int i, std::string_view s) {
        // A default-constructed string_view has data() == nullptr, which SQLite would bind as NULL: bind "" instead.
        sqlite3_bind_text(m_s, i, s.data() ? s.data() : "", static_cast<int>(s.size()), SQLITE_TRANSIENT);
        return *this;
    }
    Stmt& bind(int i, std::optional<double> d) {
        if (d) {
            sqlite3_bind_double(m_s, i, *d);
        } else {
            sqlite3_bind_null(m_s, i);
        }
        return *this;
    }
    int step() { return m_s ? sqlite3_step(m_s) : SQLITE_ERROR; }
    std::string text(int col) const {
        const auto* p = sqlite3_column_text(m_s, col);
        return p ? std::string(reinterpret_cast<const char*>(p), static_cast<std::size_t>(sqlite3_column_bytes(m_s, col))) : std::string();
    }
    std::optional<double> real(int col) const {
        if (sqlite3_column_type(m_s, col) == SQLITE_NULL) {
            return std::nullopt;
        }
        return sqlite3_column_double(m_s, col);
    }
    std::string errmsg() const { return sqlite3_errmsg(m_db); }

private:
    sqlite3* m_db;
    sqlite3_stmt* m_s = nullptr;
};

json::Value parseOrNull(const std::string& text) {
    auto v = json::parse(text);
    return v ? std::move(*v) : json::Value();
}

std::string str(const json::Value& row, const char* key) {
    const json::Value* v = row.get(key);
    return v && v->isString() ? v->asString() : std::string();
}

} // namespace

std::string sanitizeGameId(std::string_view s) {
    std::string out;
    for (const char c : s) {
        const char l = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
        out += (l >= 'a' && l <= 'z') || (l >= '0' && l <= '9') || l == '_' || l == '-' ? l : '_';
    }
    return out.empty() ? "game" : out;
}

std::string originalAssetId(std::string_view gameId, std::string_view algo, std::string_view value) {
    std::string name = sanitizeGameId(gameId);
    name += '/';
    name += algo;
    name += ':';
    name += value;
    return uuidString(uuidV5(oaidNamespace(), name));
}

struct Database::Impl {
    sqlite3* db = nullptr;
    ~Impl() {
        if (db) {
            sqlite3_close(db);
        }
    }
    bool exec(const char* sql, std::string* error) {
        char* msg = nullptr;
        if (sqlite3_exec(db, sql, nullptr, nullptr, &msg) != SQLITE_OK) {
            setError(error, std::string("sqlite: ") + (msg ? msg : "error"));
            sqlite3_free(msg);
            return false;
        }
        return true;
    }
};

Database::Database(std::unique_ptr<Impl> impl) : m_impl(std::move(impl)) {}
Database::~Database() = default;

std::unique_ptr<Database> Database::open(const fs::path& path, std::string* error) {
    auto impl = std::make_unique<Impl>();
    const auto u8 = path.u8string(); // sqlite3_open_v2 takes UTF-8
    const std::string p(u8.begin(), u8.end());
    if (sqlite3_open_v2(p.c_str(), &impl->db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) != SQLITE_OK) {
        setError(error, "sqlite: cannot open " + path.string() + ": " + (impl->db ? sqlite3_errmsg(impl->db) : "out of memory"));
        return nullptr;
    }
    if (!impl->exec("PRAGMA foreign_keys = ON;", error) || !impl->exec(kSchemaSql, error)) {
        return nullptr;
    }
    {
        Stmt s(impl->db, "SELECT value FROM meta WHERE key = 'schema_version'");
        if (s.step() == SQLITE_ROW) {
            if (std::stoi(s.text(0)) > kDbSchemaVersion) {
                setError(error, "remaster.db schema version " + s.text(0) + " is newer than this tool (" + std::to_string(kDbSchemaVersion) + ")");
                return nullptr;
            }
        } else {
            Stmt ins(impl->db, "INSERT INTO meta (key, value) VALUES ('schema_version', ?1)");
            ins.bind(1, std::to_string(kDbSchemaVersion));
            if (ins.step() != SQLITE_DONE) {
                setError(error, "sqlite: " + ins.errmsg());
                return nullptr;
            }
        }
    }
    return std::unique_ptr<Database>(new Database(std::move(impl)));
}

std::optional<std::string> Database::addOriginalAsset(std::string_view gameId, std::string_view kind, std::string_view algo,
                                                      std::string_view value, std::string_view ruleId, std::string* error) {
    if (kind.empty() || algo.empty() || value.empty()) {
        setError(error, "original_asset: kind, algo and value must not be empty");
        return std::nullopt;
    }
    const std::string oaid = originalAssetId(gameId, algo, value);
    if (const auto existing = asset(oaid)) {
        if (existing->kind != kind) {
            setError(error, "original_asset " + oaid + ": already registered as a " + existing->kind);
            return std::nullopt;
        }
    } else {
        if (const auto bound = resolve(algo, value)) {
            setError(error, "hash_key (" + std::string(algo) + ", " + std::string(value) + ") already belongs to " + *bound);
            return std::nullopt;
        }
        Stmt s(m_impl->db, "INSERT INTO original_asset (oaid, kind, game_id, first_key) VALUES (?1, ?2, ?3, ?4)");
        s.bind(1, oaid).bind(2, kind).bind(3, sanitizeGameId(gameId)).bind(4, std::string(algo) + ":" + std::string(value));
        if (s.step() != SQLITE_DONE) {
            setError(error, "sqlite: " + s.errmsg());
            return std::nullopt;
        }
    }
    if (!addHashKey(algo, value, oaid, ruleId, error)) {
        return std::nullopt;
    }
    return oaid;
}

bool Database::addHashKey(std::string_view algo, std::string_view value, std::string_view oaid, std::string_view ruleId,
                          std::string* error) {
    if (algo.empty() || value.empty()) {
        return setError(error, "hash_key: algo and value must not be empty");
    }
    if (!asset(oaid)) {
        return setError(error, "hash_key (" + std::string(algo) + ", " + std::string(value) + "): unknown oaid " + std::string(oaid));
    }
    if (const auto bound = resolve(algo, value)) {
        if (*bound != oaid) {
            return setError(error, "hash_key (" + std::string(algo) + ", " + std::string(value) + ") already belongs to " + *bound);
        }
        return true;
    }
    Stmt s(m_impl->db, "INSERT INTO hash_key (algo, value, oaid, rule_id) VALUES (?1, ?2, ?3, ?4)");
    s.bind(1, algo).bind(2, value).bind(3, oaid).bind(4, ruleId);
    if (s.step() != SQLITE_DONE) {
        return setError(error, "sqlite: " + s.errmsg());
    }
    return true;
}

std::optional<std::string> Database::resolve(std::string_view algo, std::string_view value) const {
    Stmt s(m_impl->db, "SELECT oaid FROM hash_key WHERE algo = ?1 AND value = ?2");
    s.bind(1, algo).bind(2, value);
    if (s.step() == SQLITE_ROW) {
        return s.text(0);
    }
    return std::nullopt;
}

std::optional<OriginalAsset> Database::asset(std::string_view oaid) const {
    Stmt s(m_impl->db, "SELECT oaid, kind, game_id, first_key FROM original_asset WHERE oaid = ?1");
    s.bind(1, oaid);
    if (s.step() == SQLITE_ROW) {
        return OriginalAsset{s.text(0), s.text(1), s.text(2), s.text(3)};
    }
    return std::nullopt;
}

std::vector<HashKey> Database::keysOf(std::string_view oaid) const {
    std::vector<HashKey> out;
    Stmt s(m_impl->db, "SELECT k.algo, k.value, k.oaid, k.rule_id, a.kind FROM hash_key k JOIN original_asset a ON a.oaid = k.oaid "
                       "WHERE k.oaid = ?1 ORDER BY k.algo, k.value");
    s.bind(1, oaid);
    while (s.step() == SQLITE_ROW) {
        out.push_back({s.text(0), s.text(1), s.text(2), s.text(3), s.text(4)});
    }
    return out;
}

bool Database::setReplacement(const Replacement& r, std::string* error) {
    if (!asset(r.oaid)) {
        return setError(error, "replacement " + r.pocoId + ": unknown oaid " + r.oaid);
    }
    if (r.variant.empty() || r.tier.empty() || r.pocoId.empty()) {
        return setError(error, "replacement: variant, tier and poco_id must not be empty");
    }
    Stmt s(m_impl->db, "INSERT OR REPLACE INTO replacement (oaid, variant, tier, source, poco_id, kind, fit) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7)");
    s.bind(1, r.oaid).bind(2, r.variant).bind(3, r.tier).bind(4, r.source).bind(5, r.pocoId).bind(6, r.kind).bind(7, json::writeCompact(r.fit));
    if (s.step() != SQLITE_DONE) {
        return setError(error, "sqlite: " + s.errmsg());
    }
    return true;
}

bool Database::removeReplacement(std::string_view oaid, std::string_view variant, std::string_view tier, std::string_view source) {
    Stmt s(m_impl->db, "DELETE FROM replacement WHERE oaid = ?1 AND variant = ?2 AND tier = ?3 AND source = ?4");
    s.bind(1, oaid).bind(2, variant).bind(3, tier).bind(4, source);
    return s.step() == SQLITE_DONE && sqlite3_changes(m_impl->db) > 0;
}

namespace {
std::vector<Replacement> readReplacements(sqlite3* db, const char* sql, std::optional<std::string_view> oaid) {
    std::vector<Replacement> out;
    Stmt s(db, sql);
    if (oaid) {
        s.bind(1, *oaid);
    }
    while (s.step() == SQLITE_ROW) {
        out.push_back({s.text(0), s.text(1), s.text(2), s.text(3), s.text(4), s.text(5), parseOrNull(s.text(6))});
    }
    return out;
}
} // namespace

std::vector<Replacement> Database::replacementsOf(std::string_view oaid) const {
    return readReplacements(m_impl->db,
                            "SELECT oaid, variant, tier, source, poco_id, kind, fit FROM replacement WHERE oaid = ?1 "
                            "ORDER BY oaid, variant, tier, source",
                            oaid);
}

bool Database::putRecipe(const Recipe& r, std::string* error) {
    if (r.recipeId.empty()) {
        return setError(error, "recipe: recipe_id must not be empty");
    }
    Stmt s(m_impl->db, "INSERT OR REPLACE INTO recipe (recipe_id, status, recipe) VALUES (?1, ?2, ?3)");
    s.bind(1, r.recipeId).bind(2, r.status).bind(3, json::writeCompact(r.recipe));
    if (s.step() != SQLITE_DONE) {
        return setError(error, "recipe " + r.recipeId + ": " + s.errmsg());
    }
    return true;
}

bool Database::addReview(const Review& r, std::string* error) {
    Stmt s(m_impl->db, "INSERT OR REPLACE INTO review (recipe_id, date, reviewer, decision, flip, ssim, notes) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7)");
    s.bind(1, r.recipeId).bind(2, r.date).bind(3, r.reviewer).bind(4, r.decision).bind(5, r.flip).bind(6, r.ssim).bind(7, r.notes);
    if (s.step() != SQLITE_DONE) {
        return setError(error, "review " + r.recipeId + ": " + s.errmsg());
    }
    return true;
}

std::vector<OriginalAsset> Database::assets() const {
    std::vector<OriginalAsset> out;
    Stmt s(m_impl->db, "SELECT oaid, kind, game_id, first_key FROM original_asset ORDER BY oaid");
    while (s.step() == SQLITE_ROW) {
        out.push_back({s.text(0), s.text(1), s.text(2), s.text(3)});
    }
    return out;
}

std::vector<HashKey> Database::hashKeys() const {
    std::vector<HashKey> out;
    Stmt s(m_impl->db, "SELECT k.algo, k.value, k.oaid, k.rule_id, a.kind FROM hash_key k LEFT JOIN original_asset a ON a.oaid = k.oaid "
                       "ORDER BY k.algo, k.value");
    while (s.step() == SQLITE_ROW) {
        out.push_back({s.text(0), s.text(1), s.text(2), s.text(3), s.text(4)});
    }
    return out;
}

std::vector<Replacement> Database::replacements() const {
    return readReplacements(m_impl->db,
                            "SELECT oaid, variant, tier, source, poco_id, kind, fit FROM replacement ORDER BY oaid, variant, tier, source",
                            std::nullopt);
}

std::vector<Recipe> Database::recipes() const {
    std::vector<Recipe> out;
    Stmt s(m_impl->db, "SELECT recipe_id, status, recipe FROM recipe ORDER BY recipe_id");
    while (s.step() == SQLITE_ROW) {
        out.push_back({s.text(0), s.text(1), parseOrNull(s.text(2))});
    }
    return out;
}

std::vector<Review> Database::reviews() const {
    std::vector<Review> out;
    Stmt s(m_impl->db, "SELECT recipe_id, date, reviewer, decision, notes, flip, ssim FROM review ORDER BY recipe_id, date, reviewer");
    while (s.step() == SQLITE_ROW) {
        out.push_back({s.text(0), s.text(1), s.text(2), s.text(3), s.text(4), s.real(5), s.real(6)});
    }
    return out;
}

json::Value Database::exportLock() const {
    json::Value lock = json::Value::object();
    lock["schema"] = kLockSchema;
    json::Value& oa = lock["original_asset"] = json::Value::array();
    for (const OriginalAsset& a : assets()) {
        json::Value row = json::Value::object();
        row["oaid"] = a.oaid;
        row["kind"] = a.kind;
        row["game_id"] = a.gameId;
        row["first_key"] = a.firstKey;
        oa.push(std::move(row));
    }
    json::Value& hk = lock["hash_key"] = json::Value::array();
    for (const HashKey& k : hashKeys()) {
        json::Value row = json::Value::object();
        row["algo"] = k.algo;
        row["value"] = k.value;
        row["oaid"] = k.oaid;
        row["rule_id"] = k.ruleId;
        hk.push(std::move(row));
    }
    json::Value& rp = lock["replacement"] = json::Value::array();
    for (const Replacement& r : replacements()) {
        json::Value row = json::Value::object();
        row["oaid"] = r.oaid;
        row["variant"] = r.variant;
        row["tier"] = r.tier;
        row["source"] = r.source;
        row["poco_id"] = r.pocoId;
        row["kind"] = r.kind;
        row["fit"] = r.fit;
        rp.push(std::move(row));
    }
    json::Value& rc = lock["recipe"] = json::Value::array();
    for (const Recipe& r : recipes()) {
        json::Value row = json::Value::object();
        row["recipe_id"] = r.recipeId;
        row["status"] = r.status;
        row["recipe"] = r.recipe;
        rc.push(std::move(row));
    }
    json::Value& rv = lock["review"] = json::Value::array();
    for (const Review& r : reviews()) {
        json::Value row = json::Value::object();
        row["recipe_id"] = r.recipeId;
        row["date"] = r.date;
        row["reviewer"] = r.reviewer;
        row["decision"] = r.decision;
        row["notes"] = r.notes;
        row["flip"] = r.flip ? json::Value(*r.flip) : json::Value();
        row["ssim"] = r.ssim ? json::Value(*r.ssim) : json::Value();
        rv.push(std::move(row));
    }
    return lock;
}

std::string Database::exportLockText() const { return json::writePretty(exportLock()); }

bool Database::importLock(const json::Value& lock, std::string* error) {
    if (!lock.isObject() || str(lock, "schema") != kLockSchema) {
        return setError(error, std::string("lock: not a ") + kLockSchema + " document");
    }
    if (!m_impl->exec("SAVEPOINT import_lock;", error)) {
        return false;
    }
    auto rollback = [&](const std::string& msg) {
        m_impl->exec("ROLLBACK TO import_lock; RELEASE import_lock;", nullptr);
        return setError(error, "lock: " + msg);
    };
    auto rows = [&](const char* table) -> const json::Value::Array& {
        const json::Value* t = lock.get(table);
        static const json::Value::Array kNone;
        return t ? t->asArray() : kNone;
    };
    std::string err;
    for (const json::Value& row : rows("original_asset")) {
        const std::string first = str(row, "first_key");
        const std::size_t colon = first.find(':');
        if (colon == std::string::npos) {
            return rollback("original_asset " + str(row, "oaid") + ": first_key '" + first + "' is not <algo>:<value>");
        }
        const auto oaid = addOriginalAsset(str(row, "game_id"), str(row, "kind"), first.substr(0, colon), first.substr(colon + 1), {}, &err);
        if (!oaid) {
            return rollback(err);
        }
        if (*oaid != str(row, "oaid")) {
            return rollback("original_asset " + str(row, "oaid") + ": does not recompute from its first key " + first);
        }
    }
    for (const json::Value& row : rows("hash_key")) {
        // The first key was inserted with the asset (rule ''): set its rule id now.
        if (!addHashKey(str(row, "algo"), str(row, "value"), str(row, "oaid"), str(row, "rule_id"), &err)) {
            return rollback(err);
        }
        Stmt s(m_impl->db, "UPDATE hash_key SET rule_id = ?3 WHERE algo = ?1 AND value = ?2");
        s.bind(1, str(row, "algo")).bind(2, str(row, "value")).bind(3, str(row, "rule_id"));
        s.step();
    }
    for (const json::Value& row : rows("replacement")) {
        Replacement r{str(row, "oaid"), str(row, "variant"), str(row, "tier"), str(row, "source"), str(row, "poco_id"), str(row, "kind"),
                      row.get("fit") ? *row.get("fit") : json::Value()};
        if (!setReplacement(r, &err)) {
            return rollback(err);
        }
    }
    for (const json::Value& row : rows("recipe")) {
        if (!putRecipe({str(row, "recipe_id"), str(row, "status"), row.get("recipe") ? *row.get("recipe") : json::Value()}, &err)) {
            return rollback(err);
        }
    }
    for (const json::Value& row : rows("review")) {
        Review r{str(row, "recipe_id"), str(row, "date"), str(row, "reviewer"), str(row, "decision"), str(row, "notes"), {}, {}};
        if (const json::Value* f = row.get("flip"); f && f->isNumber()) {
            r.flip = f->asDouble();
        }
        if (const json::Value* s = row.get("ssim"); s && s->isNumber()) {
            r.ssim = s->asDouble();
        }
        if (!addReview(r, &err)) {
            return rollback(err);
        }
    }
    return m_impl->exec("RELEASE import_lock;", error);
}

bool Database::saveAs(const fs::path& path, std::string* error) const {
    std::error_code ec;
    if (path.has_parent_path()) {
        fs::create_directories(path.parent_path(), ec);
    }
    fs::remove(path, ec);
    auto out = Database::open(path, error);
    if (!out) {
        return false;
    }
    if (!out->m_impl->exec("BEGIN;", error)) {
        return false;
    }
    if (!out->importLock(exportLock(), error)) {
        out->m_impl->exec("ROLLBACK;", nullptr);
        return false;
    }
    return out->m_impl->exec("COMMIT;", error);
}

std::vector<std::string> Database::verify() const {
    std::vector<std::string> errs;
    std::set<std::string> oaids;
    for (const OriginalAsset& a : assets()) {
        oaids.insert(a.oaid);
        const std::size_t colon = a.firstKey.find(':');
        if (colon == std::string::npos || originalAssetId(a.gameId, a.firstKey.substr(0, colon), a.firstKey.substr(colon + 1)) != a.oaid) {
            errs.push_back("original_asset " + a.oaid + ": does not recompute from its first key " + a.firstKey);
        } else if (resolve(a.firstKey.substr(0, colon), a.firstKey.substr(colon + 1)) != a.oaid) {
            errs.push_back("original_asset " + a.oaid + ": its first key is not one of its hash_key rows");
        }
    }
    for (const HashKey& k : hashKeys()) {
        if (!oaids.contains(k.oaid)) {
            errs.push_back("hash_key (" + k.algo + ", " + k.value + "): unknown oaid " + k.oaid);
        }
    }
    for (const Replacement& r : replacements()) {
        if (!oaids.contains(r.oaid)) {
            errs.push_back("replacement " + r.pocoId + ": unknown oaid " + r.oaid);
        }
    }
    std::set<std::string> recipeIds;
    for (const Recipe& r : recipes()) {
        recipeIds.insert(r.recipeId);
    }
    for (const Review& r : reviews()) {
        if (!recipeIds.contains(r.recipeId)) {
            errs.push_back("review of unknown recipe " + r.recipeId);
        }
    }
    return errs;
}

std::unique_ptr<Database> loadLock(std::string_view lockText, std::string* error) {
    json::ParseError pe;
    const auto v = json::parse(lockText, &pe);
    if (!v) {
        setError(error, "lock: " + pe.message + " at byte " + std::to_string(pe.offset));
        return nullptr;
    }
    auto db = Database::openMemory(error);
    if (!db || !db->importLock(*v, error)) {
        return nullptr;
    }
    return db;
}

} // namespace fuse::remaster::db
