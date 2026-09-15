#include <fuse/core/init.hpp>
#include <fuse/project/importer.hpp>
#include <fuse/project/loader.hpp>
#include <fuse/project/manifest.hpp>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

namespace {

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

void testParseManifest() {
    const char* json = R"({
  "schemaVersion": 1,
  "name": "demo_3d_empty",
  "dimensions": {
    "enable3D": true,
    "enable2D": false,
    "enableUI": false
  },
  "modules": {
    "ai": false,
    "cinematics": false,
    "fx": false,
    "mechanics": false,
    "adventure": false
  },
  "defaultWorld3D": "worlds/example.fuselevel",
  "defaultWorld2D": ""
})";

    const fuse::project::LoadResult result = fuse::project::parseManifest(json, "/tmp/demo");
    expectTrue(result.status == fuse::project::LoadStatus::Ok, "manifest parses");
    expectTrue(result.manifest.name == "demo_3d_empty", "project name");
    expectTrue(result.manifest.dimensions.enable3D, "3D enabled");
    expectTrue(!result.manifest.dimensions.enable2D, "2D disabled");
    expectTrue(result.manifest.defaultWorld3D == "worlds/example.fuselevel", "default 3D world");

    const fuse::hybrid::DimensionFlags flags =
        fuse::project::toDimensionFlags(result.manifest.dimensions);
    expectTrue(flags.enable3D && !flags.enable2D, "dimension flags mapped");
}

void testUnsupportedSchema() {
    const char* json = R"({"schemaVersion": 99, "name": "bad"})";
    const fuse::project::LoadResult result = fuse::project::parseManifest(json, "/tmp");
    expectTrue(result.status == fuse::project::LoadStatus::UnsupportedSchema, "schema rejected");
}

std::string writeTempFile(const std::string& path, const std::string& contents) {
    std::ofstream out(path, std::ios::binary);
    out << contents;
    return path;
}

void testT3DMissionImporter() {
    const std::string path = writeTempFile("/tmp/fuse_test_mission.mis",
                                           "new Scene(ExampleLevel) {\n  enabled = \"1\";\n};\n");
    const fuse::project::ImportRecord record = fuse::project::importT3DMission(path, 7u);
    expectTrue(record.ok, "mission import ok");
    expectTrue(record.worldName == "ExampleLevel", "mission name parsed");
    expectTrue(record.worldHandle.index() == 7u, "world handle assigned");
}

void testT2DModuleImporter() {
    const std::string path = writeTempFile("/tmp/fuse_test_main.cs", "module \"SpriteToy\";\n");
    const fuse::project::ImportRecord record = fuse::project::importT2DModule(path, 3u);
    expectTrue(record.ok, "module import ok");
    expectTrue(record.worldName == "SpriteToy", "module name parsed");
    expectTrue(record.worldHandle.index() == 3u, "world handle assigned");
}

void testProjectImportDryRun() {
    const char* json = R"({
  "schemaVersion": 1,
  "name": "dry_run_test",
  "dimensions": { "enable3D": true, "enable2D": true, "enableUI": false },
  "modules": { "ai": false, "cinematics": false, "fx": false, "mechanics": false, "adventure": false },
  "defaultWorld3D": "worlds/main.fuselevel",
  "defaultWorld2D": "worlds/ui.fuselevel"
})";

    const fuse::project::LoadResult loaded = fuse::project::parseManifest(json, "/tmp/proj");
    expectTrue(loaded.status == fuse::project::LoadStatus::Ok, "dry-run manifest ok");

    const fuse::project::ImportDryRunResult dryRun =
        fuse::project::importDryRun(loaded.manifest, "");
    expectTrue(dryRun.ok, "fuselevel placeholder dry-run ok");
    expectTrue(dryRun.worlds.size() == 2u, "two worlds registered");
}

} // namespace

int main() {
    fuse::core::initialize();
    testParseManifest();
    testUnsupportedSchema();
    testT3DMissionImporter();
    testT2DModuleImporter();
    testProjectImportDryRun();
    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_project_tests: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_project_tests: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
