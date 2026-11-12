// fuse_remaster: the Remaster CLI (docs/plans/FUSE_REMASTER_PLAN.md W0.7; §0.1.5, §1.3, §2.1, §3.3).
//
//   fuse_remaster init   <dir> --game <id> [--name <title>] [--force]
//       Writes a remaster project: remaster.project.json (cloud_upload "none": originals never go to a cloud API
//       until the user opts in, §0.1.5), an empty remaster.lock.json, ai-budget.json with the §7.4 defaults,
//       store/{poco,blobs}/ and a .gitignore for the derived remaster.db. Refuses to overwrite without --force.
//   fuse_remaster doctor [--project <dir>] [--json]
//       Host checks: platform, FUSE Remix availability ("unavailable (Windows only)" off Windows), GPU (CUDA
//       driver probe; "none" without one; FUSE_REMASTER_DOCTOR_NO_GPU=1 forces the no-GPU path), SQLite, and
//       with a project: the lock loads and verifies, every blob hashes to its address, cloud opt-in state.
//   fuse_remaster report [--project <dir>] [--json]
//       Counts: original assets by kind, keys by algo, replacements by tier, recipes by status, POCO records by
//       kind and distribution, blobs; lists records whose distribution is more permissive than the computed one.
//   fuse_remaster replace --project <dir> --oaid <uuid> --with <kind>/<poco id> [--tier remix] [--variant default]
//       §2.7: adds (or replaces) a replacement row pointing an original asset at a POCO record of the store (a
//       library asset, a generated asset, or an upgraded original). Refuses unknown oaids, missing records and
//       records of originals (origin original / distribution never). Rewrites the lock canonically.
//   fuse_remaster dif <file.dif>
//       W1.5: reads a Torque interior with fuse_legacy_dif (clean-room) and prints its deterministic summary JSON
//       (version, detail levels, brushes, surfaces, triangles, area, materials). Lightmaps are skipped.
//   fuse_remaster ingest-remix / export-remix (W2.3 / W2.4; built when FUSE Relight is): see remaster_remix_cli.cpp.
// Exit codes: 0 ok, 1 problems found, 2 usage / IO error.
#include <fuse/remaster/db.hpp>
#include <fuse/remaster/dif/dif.hpp>
#include <fuse/remaster/json.hpp>
#include <fuse/remaster/poco.hpp>
#include <fuse/remaster/store.hpp>

#include <sqlite3.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace fs = std::filesystem;
namespace rm = fuse::remaster;
using rm::json::Value;

namespace {

constexpr const char* kProjectSchema = "fuse.remaster-project/1";
constexpr const char* kBudgetSchema = "fuse.ai-budget/1";

struct Args {
    std::string command;
    std::vector<std::string> positional;
    std::map<std::string, std::string> options; // --key value / --flag ""
};

Args parseArgs(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        std::string s = argv[i];
        if (s.starts_with("--")) {
            const std::string key = s.substr(2);
            const bool isFlag = key == "json" || key == "force" || key == "local";
            if (!isFlag && i + 1 < argc) {
                a.options[key] = argv[++i];
            } else {
                a.options[key] = "";
            }
        } else if (a.command.empty()) {
            a.command = s;
        } else {
            a.positional.push_back(s);
        }
    }
    return a;
}

int usage() {
    std::fprintf(stderr,
                 "usage:\n"
                 "  fuse_remaster init <dir> --game <id> [--name <title>] [--force]\n"
                 "  fuse_remaster doctor [--project <dir>] [--json]\n"
                 "  fuse_remaster report [--project <dir>] [--json]\n"
                 "  fuse_remaster replace --project <dir> --oaid <uuid> --with <poco kind>/<poco id> [--tier remix]\n"
                 "                        [--variant default] [--source <name>]\n"
                 "  fuse_remaster dif <file.dif>\n"
#if defined(FUSE_REMASTER_HAVE_REMIX)
                 "  fuse_remaster ingest-remix <capture dir> --project <dir> [--rule <asset hash rule>] [--json]\n"
                 "  fuse_remaster export-remix --project <dir> --out <mod dir> [--mod-name <name>] [--local] [--json]\n"
#endif
                 );
    return 2;
}

bool writeText(const fs::path& p, const std::string& text) {
    std::string err;
    if (!rm::store::writeFileAtomic(p, std::string_view(text), &err)) {
        std::fprintf(stderr, "fuse_remaster: %s\n", err.c_str());
        return false;
    }
    return true;
}

// ---- init ---------------------------------------------------------------------------------------------------

int cmdInit(const Args& a) {
    if (a.positional.size() != 1 || !a.options.contains("game") || a.options.at("game").empty()) {
        return usage();
    }
    const fs::path dir = a.positional[0];
    const std::string game = rm::db::sanitizeGameId(a.options.at("game"));
    std::error_code ec;
    if (fs::exists(dir / "remaster.project.json", ec) && !a.options.contains("force")) {
        std::fprintf(stderr, "fuse_remaster: %s already holds a project (use --force to overwrite)\n", dir.string().c_str());
        return 2;
    }
    fs::create_directories(dir / "store" / "poco", ec);
    fs::create_directories(dir / "store" / "blobs" / "sha256", ec);

    Value project = Value::object();
    project["schema"] = kProjectSchema;
    project["game_id"] = game;
    project["name"] = a.options.contains("name") ? a.options.at("name") : game;
    project["cloud_upload"] = "none"; // §0.1.5: off by default; opting in also needs a recorded confirmation
    project["cloud_upload_confirmed"] = Value();
    project["cloud_tier"] = Value::object();
    project["store"] = "store";
    project["lock"] = "remaster.lock.json";
    project["db"] = "remaster.db";
    project["cache"] = "build/remaster-cache/" + game; // git-ignored per-game cache of originals (§0.1.2)
    project["ai_cache"] = "build/ai-cache";
    project["ai_budget"] = "ai-budget.json";
    project["audit_log"] = "build/remaster-cache/" + game + "/ai-audit.jsonl";

    Value budget = Value::object(); // §3.3 / §7.4 defaults, in the fuse_ai budget format (Tools/FUSE/Remaster/fuse_ai/budget.py)
    budget["schema"] = kBudgetSchema;
    budget["defaults"]["per_run_usd"] = 50.0;
    budget["defaults"]["cloud_monthly_usd"] = 300.0;
    budget["defaults"]["local_gpu_hours_per_run"] = 24.0;
    budget["providers"]["gemini"]["monthly_usd"] = 300.0;
    budget["providers"]["gemini"]["per_run_usd"] = 50.0;
    budget["providers"]["gemini"]["rpm"] = 60;

    auto db = rm::db::Database::openMemory();
    if (!db || !writeText(dir / "remaster.project.json", rm::json::writePretty(project)) ||
        !writeText(dir / "remaster.lock.json", db->exportLockText()) || !writeText(dir / "ai-budget.json", rm::json::writePretty(budget)) ||
        !writeText(dir / ".gitignore", "# derived from remaster.lock.json (fuse_remaster)\nremaster.db\nremaster.db-journal\n")) {
        return 2;
    }
    std::printf("initialised remaster project '%s' in %s (cloud_upload: none)\n", game.c_str(), dir.string().c_str());
    return 0;
}

// ---- shared project loading ---------------------------------------------------------------------------------

struct Project {
    fs::path dir;
    Value doc;
    std::unique_ptr<rm::db::Database> db;
    std::vector<std::string> problems;
};

std::optional<Project> loadProject(const fs::path& dir, std::string* error) {
    Project p;
    p.dir = dir;
    const auto text = rm::store::readFileText(dir / "remaster.project.json");
    if (!text) {
        *error = "no remaster.project.json in " + dir.string();
        return std::nullopt;
    }
    rm::json::ParseError pe;
    auto doc = rm::json::parse(*text, &pe);
    if (!doc || !doc->get("schema") || doc->get("schema")->asString() != kProjectSchema) {
        *error = "remaster.project.json is not a " + std::string(kProjectSchema) + " document";
        return std::nullopt;
    }
    p.doc = std::move(*doc);
    const std::string lockName = p.doc.get("lock") ? p.doc.get("lock")->asString() : "remaster.lock.json";
    const auto lock = rm::store::readFileText(dir / lockName);
    std::string err;
    if (!lock) {
        p.problems.push_back("lock: missing " + lockName);
    } else if (!(p.db = rm::db::loadLock(*lock, &err))) {
        p.problems.push_back(err);
    } else {
        for (const std::string& e : p.db->verify()) {
            p.problems.push_back("lock: " + e);
        }
        if (p.db->exportLockText() != *lock) {
            p.problems.push_back("lock: " + lockName + " is not in canonical form (re-export it)");
        }
    }
    return p;
}

std::string projectGame(const Project& p) { return p.doc.get("game_id") ? p.doc.get("game_id")->asString() : std::string(); }

fs::path storeDir(const Project& p) {
    return p.dir / (p.doc.get("store") && !p.doc.get("store")->asString().empty() ? p.doc.get("store")->asString() : std::string("store"));
}

// ---- doctor -------------------------------------------------------------------------------------------------

std::string probeGpu() {
    if (const char* off = std::getenv("FUSE_REMASTER_DOCTOR_NO_GPU"); off && *off && std::strcmp(off, "0") != 0) {
        return "none";
    }
    using CuInit = int (*)(unsigned);
    using CuDeviceGetCount = int (*)(int*);
    using CuDeviceGet = int (*)(int*, int);
    using CuDeviceGetName = int (*)(char*, int, int);
#if defined(_WIN32)
    HMODULE lib = LoadLibraryA("nvcuda.dll");
    auto sym = [&](const char* n) { return lib ? reinterpret_cast<void*>(GetProcAddress(lib, n)) : nullptr; };
#else
    void* lib = dlopen("libcuda.so.1", RTLD_NOW | RTLD_LOCAL);
    auto sym = [&](const char* n) { return lib ? dlsym(lib, n) : nullptr; };
#endif
    std::string result = "none";
    if (lib) {
        const auto init = reinterpret_cast<CuInit>(sym("cuInit"));
        const auto count = reinterpret_cast<CuDeviceGetCount>(sym("cuDeviceGetCount"));
        const auto get = reinterpret_cast<CuDeviceGet>(sym("cuDeviceGet"));
        const auto name = reinterpret_cast<CuDeviceGetName>(sym("cuDeviceGetName"));
        int n = 0;
        if (init && count && get && name && init(0) == 0 && count(&n) == 0 && n > 0) {
            int dev = 0;
            char buf[256] = {};
            if (get(&dev, 0) == 0 && name(buf, sizeof(buf) - 1, dev) == 0) {
                result = std::string(buf) + " (CUDA, " + std::to_string(n) + " device" + (n > 1 ? "s" : "") + ")";
            }
        }
#if defined(_WIN32)
        FreeLibrary(lib);
#else
        dlclose(lib);
#endif
    }
    return result;
}

int cmdDoctor(const Args& a) {
    std::vector<std::pair<std::string, std::string>> rows;
    bool ok = true;
#if defined(_WIN32)
    rows.emplace_back("platform", "windows");
    rows.emplace_back("fuse-remix", "host supported (Windows); runtime availability is checked by FUSE Relight");
#elif defined(__linux__)
    rows.emplace_back("platform", "linux");
    rows.emplace_back("fuse-remix", "unavailable (Windows only)");
#else
    rows.emplace_back("platform", "other");
    rows.emplace_back("fuse-remix", "unavailable (Windows only)");
#endif
    rows.emplace_back("gpu", probeGpu());
    rows.emplace_back("sqlite", sqlite3_libversion());
    if (a.options.contains("project")) {
        std::string err;
        auto p = loadProject(a.options.at("project"), &err);
        if (!p) {
            rows.emplace_back("project", "error: " + err);
            ok = false;
        } else {
            rows.emplace_back("project", projectGame(*p));
            const std::string cloud = p->doc.get("cloud_upload") ? p->doc.get("cloud_upload")->asString() : "none";
            const bool confirmed = p->doc.get("cloud_upload_confirmed") && p->doc.get("cloud_upload_confirmed")->isObject();
            rows.emplace_back("cloud_upload", cloud == "opt-in" ? (confirmed ? "opt-in (confirmed)" : "opt-in (NOT confirmed: originals stay local)")
                                                                : "none (originals never leave the machine)");
            if (cloud != "none" && cloud != "opt-in") {
                p->problems.push_back("cloud_upload must be \"none\" or \"opt-in\"");
            }
            const rm::store::BlobStore store(storeDir(*p));
            std::size_t bad = 0;
            const auto blobs = store.list();
            for (const std::string& sha : blobs) {
                const auto bytes = store.get(sha);
                if (!bytes || rm::sha256Hex(bytes->data(), bytes->size()) != sha) {
                    p->problems.push_back("blob " + sha + " does not hash to its address");
                    ++bad;
                }
            }
            rows.emplace_back("blobs", std::to_string(blobs.size()) + " (" + std::to_string(bad) + " bad)");
            rows.emplace_back("lock", p->db ? std::to_string(p->db->assets().size()) + " original assets, " +
                                                  std::to_string(p->db->hashKeys().size()) + " keys"
                                            : "unreadable");
            for (const std::string& e : p->problems) {
                rows.emplace_back("problem", e);
                ok = false;
            }
        }
    }
    if (a.options.contains("json")) {
        Value out = Value::object();
        Value problems = Value::array();
        for (const auto& [k, v] : rows) {
            if (k == "problem") {
                problems.push(v);
            } else {
                out[k] = v;
            }
        }
        out["problems"] = std::move(problems);
        out["ok"] = ok;
        std::fputs(rm::json::writePretty(out).c_str(), stdout);
    } else {
        for (const auto& [k, v] : rows) {
            std::printf("%s: %s\n", k.c_str(), v.c_str());
        }
        std::printf("%s\n", ok ? "doctor: ok" : "doctor: problems found");
    }
    return ok ? 0 : 1;
}

// ---- report -------------------------------------------------------------------------------------------------

int cmdReport(const Args& a) {
    const fs::path dir = a.options.contains("project") ? fs::path(a.options.at("project")) : fs::current_path();
    std::string err;
    auto p = loadProject(dir, &err);
    if (!p) {
        std::fprintf(stderr, "fuse_remaster: %s\n", err.c_str());
        return 2;
    }
    std::map<std::string, std::int64_t> assetsByKind, keysByAlgo, replByTier, recipesByStatus, recordsByKind, recordsByDist;
    if (p->db) {
        for (const auto& x : p->db->assets()) {
            ++assetsByKind[x.kind];
        }
        for (const auto& x : p->db->hashKeys()) {
            ++keysByAlgo[x.algo];
        }
        for (const auto& x : p->db->replacements()) {
            ++replByTier[x.tier];
        }
        for (const auto& x : p->db->recipes()) {
            ++recipesByStatus[x.status];
        }
    }
    const rm::store::BlobStore store(storeDir(*p));
    Value violations = Value::array();
    for (const std::string& rel : store.listRecords()) {
        const auto text = rm::store::readFileText(store.root() / rel);
        std::string perr;
        const auto rec = text ? rm::poco::fromJsonText(*text, &perr) : std::nullopt;
        if (!rec) {
            p->problems.push_back(rel + ": " + perr);
            continue;
        }
        ++recordsByKind[rec->kindName()];
        ++recordsByDist[rm::poco::enumName(rec->distribution)];
        for (const std::string& e : rm::poco::validate(*rec)) {
            violations.push(rel + ": " + e);
        }
        if (rel != rm::poco::recordPath(*rec)) {
            violations.push(rel + ": path does not match its kind and id");
        }
    }
    std::uint64_t blobBytes = 0;
    const auto blobs = store.list();
    for (const std::string& sha : blobs) {
        std::error_code ec;
        blobBytes += fs::file_size(store.blobPath(sha), ec);
    }
    auto counts = [](const std::map<std::string, std::int64_t>& m) {
        Value o = Value::object();
        for (const auto& [k, n] : m) {
            o[k] = n;
        }
        return o;
    };
    Value out = Value::object();
    out["game_id"] = projectGame(*p);
    out["original_assets"] = counts(assetsByKind);
    out["hash_keys"] = counts(keysByAlgo);
    out["replacements"] = counts(replByTier);
    out["recipes"] = counts(recipesByStatus);
    out["records"] = counts(recordsByKind);
    out["distribution"] = counts(recordsByDist);
    out["blobs"]["count"] = static_cast<std::uint64_t>(blobs.size());
    out["blobs"]["bytes"] = blobBytes;
    out["violations"] = violations;
    Value problems = Value::array();
    for (const std::string& e : p->problems) {
        problems.push(e);
    }
    out["problems"] = problems;
    const bool ok = violations.size() == 0 && problems.size() == 0;
    if (a.options.contains("json")) {
        std::fputs(rm::json::writePretty(out).c_str(), stdout);
    } else {
        std::printf("project %s\n", projectGame(*p).c_str());
        for (const char* section : {"original_assets", "hash_keys", "replacements", "recipes", "records", "distribution"}) {
            std::printf("  %s:", section);
            if (out[section].size() == 0) {
                std::printf(" none");
            }
            for (const auto& [k, n] : out[section].asObject()) {
                std::printf(" %s=%lld", k.c_str(), static_cast<long long>(n.asInt()));
            }
            std::printf("\n");
        }
        std::printf("  blobs: %zu (%llu bytes)\n", blobs.size(), static_cast<unsigned long long>(blobBytes));
        for (const auto& v : violations.asArray()) {
            std::printf("  violation: %s\n", v.asString().c_str());
        }
        for (const auto& v : problems.asArray()) {
            std::printf("  problem: %s\n", v.asString().c_str());
        }
    }
    return ok ? 0 : 1;
}

// ---- replace (§2.7) ------------------------------------------------------------------------------------------

int cmdReplace(const Args& a) {
    if (!a.options.contains("project") || !a.options.contains("oaid") || !a.options.contains("with")) {
        return usage();
    }
    std::string err;
    auto p = loadProject(a.options.at("project"), &err);
    if (!p || !p->db) {
        std::fprintf(stderr, "fuse_remaster: %s\n", p ? "the project lock does not load" : err.c_str());
        return 2;
    }
    const std::string with = a.options.at("with");
    const auto slash = with.find('/');
    if (slash == std::string::npos) {
        std::fprintf(stderr, "fuse_remaster: --with takes <kind>/<poco id> (e.g. mesh/lib/crate)\n");
        return 2;
    }
    const std::string kind = with.substr(0, slash), id = with.substr(slash + 1);
    const std::string oaid = a.options.at("oaid");
    if (!p->db->asset(oaid)) {
        std::fprintf(stderr, "fuse_remaster: no original asset %s in the lock\n", oaid.c_str());
        return 1;
    }
    const rm::store::BlobStore store(storeDir(*p));
    const auto rec = store.readRecord(kind, id, &err);
    if (!rec) {
        std::fprintf(stderr, "fuse_remaster: no record %s: %s\n", with.c_str(), err.c_str());
        return 1;
    }
    if (rec->provenance.origin == rm::poco::Origin::Original || rec->distribution == rm::poco::Distribution::Never ||
        rm::poco::computeDistribution(*rec) == rm::poco::Distribution::Never) {
        std::fprintf(stderr, "fuse_remaster: %s is an original (or never distributed): it cannot be a replacement\n", with.c_str());
        return 1;
    }
    rm::db::Replacement r;
    r.oaid = oaid;
    r.variant = a.options.contains("variant") ? a.options.at("variant") : "default";
    r.tier = a.options.contains("tier") ? a.options.at("tier") : "remix";
    r.source = a.options.contains("source") ? a.options.at("source") : "";
    r.pocoId = id;
    r.kind = kind;
    if (!p->db->setReplacement(r, &err)) {
        std::fprintf(stderr, "fuse_remaster: %s\n", err.c_str());
        return 1;
    }
    const std::string lockName = p->doc.get("lock") ? p->doc.get("lock")->asString() : "remaster.lock.json";
    if (!writeText(p->dir / lockName, p->db->exportLockText())) {
        return 2;
    }
    std::printf("replacement %s -> %s (%s, %s)\n", oaid.c_str(), with.c_str(), r.tier.c_str(), r.variant.c_str());
    return 0;
}

// ---- dif (W1.5) ---------------------------------------------------------------------------------------------

int cmdDif(const Args& a) {
    if (a.positional.size() != 1) {
        return usage();
    }
    const auto bytes = rm::store::readFileBytes(a.positional[0]);
    if (!bytes) {
        std::fprintf(stderr, "fuse_remaster: cannot read %s\n", a.positional[0].c_str());
        return 2;
    }
    const auto result = fuse::remaster::dif::readDif(std::span<const std::uint8_t>(bytes->data(), bytes->size()));
    if (!result.ok) {
        std::fprintf(stderr, "fuse_remaster: %s: %s\n", a.positional[0].c_str(), result.error.c_str());
        return 1;
    }
    for (const std::string& w : result.warnings) {
        std::fprintf(stderr, "fuse_remaster: warning: %s\n", w.c_str());
    }
    std::fputs(fuse::remaster::dif::summaryJson(result.resource).c_str(), stdout);
    return 0;
}

} // namespace

#if defined(FUSE_REMASTER_HAVE_REMIX)
int fuseRemasterIngestRemix(int argc, char** argv); // remix/src/remaster_remix_cli.cpp
int fuseRemasterExportRemix(int argc, char** argv);
#endif

int main(int argc, char** argv) {
    const Args a = parseArgs(argc, argv);
    if (a.command == "init") {
        return cmdInit(a);
    }
    if (a.command == "doctor") {
        return cmdDoctor(a);
    }
    if (a.command == "report") {
        return cmdReport(a);
    }
    if (a.command == "replace") {
        return cmdReplace(a);
    }
    if (a.command == "dif") {
        return cmdDif(a);
    }
#if defined(FUSE_REMASTER_HAVE_REMIX)
    if (a.command == "ingest-remix") {
        return fuseRemasterIngestRemix(argc, argv);
    }
    if (a.command == "export-remix") {
        return fuseRemasterExportRemix(argc, argv);
    }
#endif
    return usage();
}
