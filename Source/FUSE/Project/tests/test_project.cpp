#include <fuse/core/init.hpp>
#include <fuse/project/importer.hpp>
#include <fuse/project/importer_extract.hpp>
#include <fuse/project/loader.hpp>
#include <fuse/jobs/worker_count.hpp>
#include <fuse/platform/power.hpp>
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

void testWorkerCapManifest() {
    const char* json = R"({
  "schemaVersion": 1,
  "name": "worker_cap_test",
  "dimensions": { "enable3D": true, "enable2D": false, "enableUI": false },
  "modules": { "ai": false, "cinematics": false, "fx": false, "mechanics": false, "adventure": false },
  "defaultWorld3D": "",
  "defaultWorld2D": "",
  "workerCap": 4
})";

    const fuse::project::LoadResult result = fuse::project::parseManifest(json, "/tmp");
    expectTrue(result.status == fuse::project::LoadStatus::Ok, "workerCap manifest parses");
    expectTrue(result.manifest.workerCap == 4u, "workerCap value preserved");

    fuse::jobs::setProjectWorkerCap(result.manifest.workerCap);
    fuse::jobs::WorkerCountParams params;
    params.usableCores = 32;
    params.mobileProfile = false;
    params.powerState = fuse::platform::PowerState::Normal;
    params.hardCap = fuse::jobs::projectWorkerCap();
    expectTrue(fuse::jobs::computeWorkerCount(params) == 4u, "project workerCap applied");
    fuse::jobs::setProjectWorkerCap(0);
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
                                           R"(new Scene(ExampleLevel) {
  new GroundPlane() {
    MaterialAsset = "Prototyping:FloorGray";
    position = "0 0 0";
    rotation = "1 0 0 0";
    scale = "1 1 1";
  };
  new SimGroup(CameraSpawnPoints) {
    new SpawnSphere(DefaultCameraSpawnSphere) {
      dataBlock = "SpawnSphereMarker";
      position = "0 0 10";
    };
  };
};)");
    const fuse::project::ImportRecord record = fuse::project::importT3DMission(path, 7u);
    expectTrue(record.ok, "mission import ok");
    expectTrue(record.worldName == "ExampleLevel", "mission name parsed");
    expectTrue(record.worldHandle.index() == 7u, "world handle assigned");
    expectTrue(record.t3dExtract.simObjects.size() >= 4u, "simobjects extracted");
    expectTrue(record.t3dExtract.materials.size() == 1u, "material asset extracted");
    expectTrue(record.t3dExtract.datablocks.size() == 1u, "datablock extracted");
}

void testT2DModuleImporter() {
    const std::string path = writeTempFile("/tmp/fuse_test_main.cs",
                                           R"(module "SpriteToy";
new SceneToy() {
  new SpritePlayer(Player) {
    position = "0 0";
  };
};)");
    const fuse::project::ImportRecord record = fuse::project::importT2DModule(path, 3u);
    expectTrue(record.ok, "module import ok");
    expectTrue(record.worldName == "SpriteToy", "module name parsed");
    expectTrue(record.worldHandle.index() == 3u, "world handle assigned");
    expectTrue(record.t2dExtract.sceneNodes.size() >= 2u, "t2d scene nodes extracted");
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
    testWorkerCapManifest();
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
