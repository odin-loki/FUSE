// MP-B7.3-SCRIPT-COMPONENT: CookAssetKind::Script through AssetCooker / the cook manifest / the job graph.
//  - `.lua` cooks to a Lua-bytecode `.fusescript`; the cache serves the second cook
//  - a syntax error is rejected with CookStatus::ScriptSyntaxError and a file:line message, nothing written
//  - legacy `.cs` is tagged `t3d:` and passed through
//  - the manifest parser and cook cache round-trip the "script" kind
#include <fuse/project/asset_cooker.hpp>
#include <fuse/project/cook_cache.hpp>
#include <fuse/project/cook_manifest.hpp>
#include <fuse/script/script_cook.hpp>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
using fuse::project::AssetCooker;
using fuse::project::CookAssetKind;
using fuse::project::CookManifest;
using fuse::project::CookManifestEntry;
using fuse::project::CookRecord;
using fuse::project::CookStatus;

namespace {

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

CookManifestEntry scriptEntry(const fs::path& source, const fs::path& output) {
    CookManifestEntry entry;
    entry.kind = CookAssetKind::Script;
    entry.source_path = source.string();
    entry.output_path = output.string();
    return entry;
}

} // namespace

int main() {
    if (!fuse::script::script_compiler_available()) {
        std::printf("fuse_script_cook: SKIP (no Lua compiler in this build)\n");
        return 77;
    }
    std::error_code ec;
    const fs::path dir = fs::temp_directory_path(ec) / "fuse_script_cook_test";
    fs::remove_all(dir, ec);
    fs::create_directories(dir / "cooked", ec);

    expectTrue(std::string(fuse::project::cookAssetKindName(CookAssetKind::Script)) == "script", "kind name");
    CookAssetKind parsed = CookAssetKind::Mesh;
    expectTrue(fuse::project::parseCookAssetKindName("script", parsed) && parsed == CookAssetKind::Script,
               "kind name parses back");
    expectTrue(fuse::project::isImportValidationFailure(CookStatus::ScriptSyntaxError) &&
                   std::string(fuse::project::cookStatusName(CookStatus::ScriptSyntaxError)) == "script_syntax_error",
               "ScriptSyntaxError is a named import-validation failure");

    const fs::path good = dir / "door.lua";
    std::ofstream(good) << "function on_update(self, dt) self.t = (self.t or 0) + dt end\n";
    const fs::path bad = dir / "broken.lua";
    std::ofstream(bad) << "function on_start(self)\n  if self then\nend\n";
    const fs::path legacy = dir / "main.cs";
    std::ofstream(legacy) << "function onStart() { echo(\"hi\"); }\n";

    AssetCooker cooker;
    const CookRecord first = cooker.cook_entry(scriptEntry(good, dir / "cooked" / "door.fusescript"));
    expectTrue(first.ok && first.status == CookStatus::Ok && !first.cache_hit, "Lua script cooks");
    fuse::script::CookedScript cooked;
    expectTrue(fuse::script::load_cooked_script((dir / "cooked" / "door.fusescript").string(), cooked) &&
                   cooked.kind == fuse::script::CookedScriptKind::LuaBytecode,
               "output is a Lua-bytecode .fusescript");
    const CookRecord second = cooker.cook_entry(scriptEntry(good, dir / "cooked" / "door.fusescript"));
    expectTrue(second.ok && second.cache_hit, "unchanged script is a cache hit");

    const CookRecord rejected = cooker.cook_entry(scriptEntry(bad, dir / "cooked" / "broken.fusescript"));
    expectTrue(!rejected.ok && rejected.status == CookStatus::ScriptSyntaxError, "syntax error rejects the cook");
    expectTrue(rejected.note.find("Lua syntax error") != std::string::npos &&
                   rejected.note.find("broken.lua:") != std::string::npos,
               "rejection note names the file and line");
    expectTrue(!fs::exists(dir / "cooked" / "broken.fusescript"), "rejected script writes no output");
    std::printf("rejected: %s\n", rejected.note.c_str());

    const CookRecord passthrough = cooker.cook_entry(scriptEntry(legacy, dir / "cooked" / "main.fusescript"));
    fuse::script::CookedScript legacyCooked;
    expectTrue(passthrough.ok &&
                   fuse::script::load_cooked_script((dir / "cooked" / "main.fusescript").string(), legacyCooked) &&
                   legacyCooked.kind == fuse::script::CookedScriptKind::LegacyTorqueScript &&
                   legacyCooked.chunk_name == "t3d:main",
               "legacy .cs passes through tagged t3d:");

    // Manifest JSON with "kind": "script" + job-graph cook keeps the specific failure status.
    const std::string json = std::string("{\"schemaVersion\": 1, \"assets\": [") + "{\"kind\": \"script\", \"sourcePath\": \"" +
                             fs::path(bad).generic_string() + "\", \"outputPath\": \"" +
                             (dir / "cooked" / "broken2.fusescript").generic_string() + "\"}]}";
    const fuse::project::CookManifestLoadResult manifest = fuse::project::parseCookManifest(json, dir.string());
    expectTrue(manifest.status == fuse::project::CookManifestLoadStatus::Ok && manifest.manifest.assets.size() == 1u &&
                   manifest.manifest.assets[0].kind == CookAssetKind::Script,
               "manifest parses a script asset");
    AssetCooker graphCooker;
    const fuse::project::CookBatchResult batch = graphCooker.cook_manifest(manifest.manifest);
    expectTrue(!batch.ok && batch.records.size() == 1u && batch.records[0].status == CookStatus::ScriptSyntaxError,
               "job-graph cook reports ScriptSyntaxError");

    // Cook cache JSON round trip keeps the script kind.
    const std::string cachePath = (dir / "cache.json").string();
    expectTrue(cooker.cache().save(cachePath), "cook cache saves");
    fuse::project::CookCache reloaded;
    expectTrue(reloaded.load(cachePath) && reloaded.entry_count() >= 2u && reloaded.contains(first.content_hash),
               "cook cache reloads the script entries");
    // A wrongly parsed kind would recompute a different (mesh) key and read as stale.
    expectTrue(reloaded.count_stale_entries() == 0u, "reloaded cache entries keep kind=script (not stale)");

    fs::remove_all(dir, ec);
    if (g_failures != 0) {
        std::fprintf(stderr, "fuse_script_cook: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("fuse_script_cook: PASS\n");
    return 0;
}
