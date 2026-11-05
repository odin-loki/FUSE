// FUSE Remaster W0.3 gate remaster_db (plan §7 Wave 0, W0.3 exit criteria):
//   * multi-key -> one oaid: sha256, remix64, legacy-path and dts-node keys of one original resolve to the same
//     oaid; a key bound to another original is refused; oaids are the pinned UUIDv5 values (Python's uuid5 agrees);
//   * deterministic export: the lock is byte-identical when exported twice, when the rows were inserted in another
//     order, after loadLock(lock) and after a save / reopen of remaster.db; saveAs writes byte-identical files;
//   * importLock is all-or-nothing; verify() catches a tampered oaid.
// Usage: remaster_db_tests <scratch dir>
#include "check.hpp"

#include <fuse/remaster/db.hpp>
#include <fuse/remaster/digest.hpp>
#include <fuse/remaster/store.hpp>

#include <algorithm>
#include <filesystem>

namespace db = fuse::remaster::db;
namespace json = fuse::remaster::json;
namespace fs = std::filesystem;

namespace {

struct Row {
    const char* kind;
    const char* algo;
    const char* value;
};

void populate(db::Database& d, bool reversed) {
    std::vector<Row> firsts = {{"texture", "sha256", "aa00000000000000000000000000000000000000000000000000000000000001"},
                               {"mesh", "sha256", "bb00000000000000000000000000000000000000000000000000000000000002"},
                               {"sound", "sha256", "cc00000000000000000000000000000000000000000000000000000000000003"}};
    if (reversed) {
        std::reverse(firsts.begin(), firsts.end());
    }
    std::vector<std::string> oaids;
    for (const Row& r : firsts) {
        std::string err;
        const auto oaid = d.addOriginalAsset("Test Game", r.kind, r.algo, r.value, "", &err);
        CHECK(oaid.has_value());
        oaids.push_back(oaid.value_or(""));
    }
    const std::string tex = db::originalAssetId("test_game", "sha256", firsts[reversed ? 2 : 0].value);
    const std::string mesh = db::originalAssetId("test_game", "sha256", "bb00000000000000000000000000000000000000000000000000000000000002");
    std::vector<std::tuple<std::string, std::string, std::string, std::string>> keys = {
        {"remix64", "0123456789ABCDEF", tex, "rule-A"},
        {"legacy-path", "textures/castle/wall.png@data.zip", tex, ""},
        {"fuse-capture64", "FEDCBA9876543210", tex, ""},
        {"dts-node", "bb00/detail0/mesh2", mesh, ""},
        {"remix64", "1111111111111111", mesh, "rule-B"},
    };
    if (reversed) {
        std::reverse(keys.begin(), keys.end());
    }
    for (const auto& [algo, value, oaid, rule] : keys) {
        std::string err;
        CHECK(d.addHashKey(algo, value, oaid, rule, &err));
    }
    std::string err;
    CHECK(d.setReplacement({tex, "default", "T2", "", "tex/castle/wall_stone_01", "texture_set", json::Value()}, &err));
    json::Value fit = json::Value::object();
    fit["scale"] = 0.0254;
    fit["pivot"] = "bottom";
    CHECK(d.setReplacement({mesh, "lowspec", "remix", "mods/a", "mesh/prop/barrel_a", "mesh", fit}, &err));
    json::Value recipe = json::Value::object();
    recipe["schema"] = "fuse.ai-recipe/1";
    recipe["task"] = "UPSCALE";
    CHECK(d.putRecipe({"tex-upscale/aa00", "approved", recipe}, &err));
    CHECK(d.addReview({"tex-upscale/aa00", "2026-10-02", "odin", "approved", "crisp", 0.004, 0.97}, &err));
}

void testKeys() {
    auto d = db::Database::openMemory();
    CHECK(d != nullptr);
    if (!d) {
        return;
    }
    populate(*d, false);
    const std::string tex = db::originalAssetId("Test Game", "sha256", "aa00000000000000000000000000000000000000000000000000000000000001");
    // Multi-key -> one oaid.
    for (const auto& [algo, value] : std::vector<std::pair<std::string, std::string>>{
             {"sha256", "aa00000000000000000000000000000000000000000000000000000000000001"},
             {"remix64", "0123456789ABCDEF"},
             {"legacy-path", "textures/castle/wall.png@data.zip"},
             {"fuse-capture64", "FEDCBA9876543210"}}) {
        CHECK(d->resolve(algo, value) == tex);
    }
    CHECK(d->keysOf(tex).size() == 4);
    CHECK(d->keysOf(tex).front().kind == "texture");
    CHECK(!d->resolve("remix64", "unknown"));
    // One key, one original.
    std::string err;
    const std::string mesh = *d->resolve("dts-node", "bb00/detail0/mesh2");
    CHECK(!d->addHashKey("remix64", "0123456789ABCDEF", mesh, "", &err) && err.find("already belongs") != std::string::npos);
    CHECK(d->addHashKey("remix64", "0123456789ABCDEF", tex, "rule-A", &err)); // idempotent
    CHECK(!d->addHashKey("remix64", "2222", "no-such-oaid", "", &err));
    // Registering by a key that already belongs to another asset fails; the same asset again is idempotent.
    CHECK(!d->addOriginalAsset("Test Game", "texture", "remix64", "1111111111111111", "", &err));
    CHECK(d->addOriginalAsset("Test Game", "texture", "sha256", "aa00000000000000000000000000000000000000000000000000000000000001") == tex);
    CHECK(!d->addOriginalAsset("Test Game", "mesh", "sha256", "aa00000000000000000000000000000000000000000000000000000000000001", "", &err));
    // Replacement rows need a known original.
    CHECK(!d->setReplacement({"nope", "default", "T0", "", "x", "", json::Value()}, &err));
    CHECK(d->replacementsOf(mesh).size() == 1 && d->replacementsOf(mesh).front().fit.get("pivot")->asString() == "bottom");
    CHECK(d->verify().empty());
    // Pinned oaids (Python: uuid.uuid5(uuid.uuid5(uuid.NAMESPACE_URL, "https://fuse.invalid/remaster/oaid"),
    // "test_game/remix.texture:0123456789ABCDEF")) and the RFC 4122 / Python reference vector.
    CHECK(db::originalAssetId("Test Game", "remix.texture", "0123456789ABCDEF") == "475efc0e-3782-50ef-8382-233cc7da749d");
    const fuse::remaster::Uuid nsDns = {0x6b, 0xa7, 0xb8, 0x10, 0x9d, 0xad, 0x11, 0xd1, 0x80, 0xb4, 0x00, 0xc0, 0x4f, 0xd4, 0x30, 0xc8};
    CHECK(fuse::remaster::uuidString(fuse::remaster::uuidV5(nsDns, "python.org")) == "886313e1-3b8a-5372-9b90-0c9aee199e5d");
    CHECK(db::sanitizeGameId("Test Game!") == "test_game_" && db::sanitizeGameId("") == "game");
}

void testExport(const fs::path& dir) {
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    auto a = db::Database::openMemory();
    auto b = db::Database::openMemory();
    populate(*a, false);
    populate(*b, true);
    const std::string lockA = a->exportLockText();
    CHECK(lockA == a->exportLockText());           // twice
    CHECK(lockA == b->exportLockText());           // insertion order does not matter
    std::string err;
    auto c = db::loadLock(lockA, &err);            // lock -> DB -> lock
    if (!c) {
        std::fprintf(stderr, "  loadLock: %s\n", err.c_str());
    }
    CHECK(c && c->exportLockText() == lockA);
    // The .db files: saveAs twice gives the same bytes; reopening gives the same lock.
    CHECK(a->saveAs(dir / "a.db", &err));
    CHECK(b->saveAs(dir / "b.db", &err));
    CHECK(a->saveAs(dir / "a.db", &err)); // overwrite
    const auto fa = fuse::remaster::store::readFileBytes(dir / "a.db");
    const auto fb = fuse::remaster::store::readFileBytes(dir / "b.db");
    CHECK(fa && fb && !fa->empty() && *fa == *fb);
    {
        auto reopened = db::Database::open(dir / "a.db", &err);
        CHECK(reopened && reopened->exportLockText() == lockA && reopened->verify().empty());
    }
    // Lock shape.
    const auto doc = json::parse(lockA);
    CHECK(doc && doc->get("schema")->asString() == db::kLockSchema);
    CHECK(doc->get("original_asset")->size() == 3 && doc->get("hash_key")->size() == 8 && doc->get("replacement")->size() == 2 &&
          doc->get("recipe")->size() == 1 && doc->get("review")->size() == 1);
    // importLock is all-or-nothing: a lock whose last key conflicts leaves the DB unchanged.
    {
        json::Value bad = *doc;
        json::Value row = json::Value::object();
        row["algo"] = "remix64";
        row["value"] = "0123456789ABCDEF"; // already bound to the texture
        row["oaid"] = bad["original_asset"].arr()[0]["oaid"].asString() ==
                              db::originalAssetId("test_game", "sha256", "aa00000000000000000000000000000000000000000000000000000000000001")
                          ? bad["original_asset"].arr()[1]["oaid"]
                          : bad["original_asset"].arr()[0]["oaid"];
        row["rule_id"] = "";
        bad["hash_key"].push(row);
        auto e = db::Database::openMemory();
        CHECK(!e->importLock(bad, &err) && !err.empty());
        CHECK(e->assets().empty() && e->hashKeys().empty());
    }
    // A tampered oaid is refused on import.
    {
        json::Value bad = *doc;
        bad["original_asset"].arr()[0]["oaid"] = "00000000-0000-5000-8000-000000000000";
        CHECK(!db::loadLock(json::writePretty(bad), &err));
        CHECK(err.find("does not recompute") != std::string::npos);
    }
    CHECK(!db::loadLock("{\"schema\":\"other\"}", &err));
    fs::remove_all(dir, ec);
}

} // namespace

int main(int argc, char** argv) {
    testKeys();
    testExport(argc > 1 ? fs::path(argv[1]) : fs::current_path() / "remaster_db_tmp");
    return remaster_test::finish("remaster_db");
}
