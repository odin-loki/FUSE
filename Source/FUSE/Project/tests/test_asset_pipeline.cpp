#include <fuse/core/init.hpp>
#include <fuse/project/asset_cooker.hpp>
#include <fuse/project/asset_graph.hpp>
#include <fuse/project/cook_manifest.hpp>
#include <fuse/project/import_pipeline.hpp>
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

std::string writeTempFile(const std::string& path, const std::string& contents) {
    std::ofstream out(path, std::ios::binary);
    out << contents;
    return path;
}

void testParseCookManifest() {
    const char* json = R"({
  "schemaVersion": 1,
  "assets": [
    {
      "kind": "mesh",
      "sourcePath": "art/hero.obj",
      "outputPath": "cooked/hero.fusemesh"
    },
    {
      "kind": "texture",
      "sourcePath": "art/albedo.png",
      "outputPath": "cooked/albedo.fusetex"
    }
  ]
})";

    const fuse::project::CookManifestLoadResult result =
        fuse::project::parseCookManifest(json, "/tmp/demo");
    expectTrue(result.status == fuse::project::CookManifestLoadStatus::Ok, "cook manifest parses");
    expectTrue(result.manifest.assets.size() == 2u, "two assets parsed");
    expectTrue(result.manifest.assets[0].kind == fuse::project::CookAssetKind::Mesh, "first asset is mesh");
    expectTrue(result.manifest.assets[1].source_path == "art/albedo.png", "texture source path");
}

void testAssetGraphRoundTrip() {
    fuse::project::AssetGraph graph;
    graph.add_asset("cooked/a.fusemesh", "/tmp/fuse_b79_source_a.obj");
    graph.add_dependency("cooked/a.fusemesh", "/tmp/fuse_b79_dep.mtl");

    const std::string graphPath = "/tmp/fuse_b79_graph.json";
    expectTrue(graph.save(graphPath), "asset graph saves");

    fuse::project::AssetGraph loaded;
    expectTrue(loaded.load(graphPath), "asset graph loads");
    expectTrue(loaded.asset_count() == 1u, "one asset round-tripped");
}

void testAssetCookerStub() {
    const std::string source = writeTempFile("/tmp/fuse_b79_mesh.obj", "# stub mesh\n");
    fuse::project::MeshImportDesc desc;
    desc.input_path = source;
    desc.output_path = "/tmp/fuse_b79_mesh.fusemesh";

    fuse::project::AssetCooker cooker;
    const fuse::project::CookRecord record = cooker.cook_mesh(desc);
    expectTrue(record.ok, "mesh cook stub ok");
    expectTrue(record.status == fuse::project::CookStatus::Ok, "mesh cook status ok");
}

void testImportPipelineDryRun() {
    fuse::project::CookManifest manifest = fuse::project::makeDefaultCookManifest("/tmp/demo");
    fuse::project::ImportPipeline pipeline;
    pipeline.set_project_root("/tmp/demo");

    const fuse::project::CookBatchResult planned = pipeline.plan_from_manifest(manifest);
    expectTrue(planned.ok, "pipeline plan ok");
    expectTrue(planned.records.size() == manifest.assets.size(), "planned all manifest assets");

    const fuse::project::CookBatchResult dryRun = pipeline.execute(true);
    expectTrue(dryRun.ok, "pipeline dry-run ok");
}

void testPlanForProject() {
    const char* json = R"({
  "schemaVersion": 1,
  "name": "demo_3d_empty",
  "dimensions": { "enable3D": true, "enable2D": false, "enableUI": false },
  "modules": { "ai": false, "cinematics": false, "fx": false, "mechanics": false, "adventure": false },
  "defaultWorld3D": "worlds/example.fuselevel",
  "defaultWorld2D": ""
})";

    const fuse::project::LoadResult loaded = fuse::project::parseManifest(json, "/tmp/demo");
    expectTrue(loaded.status == fuse::project::LoadStatus::Ok, "project manifest ok");

    const fuse::project::CookBatchResult planned =
        fuse::project::ImportPipeline::planForProject(loaded.manifest, "/tmp/demo");
    expectTrue(planned.ok, "project cook plan ok");
    expectTrue(planned.records.size() >= 2u, "default manifest plus world entry");
}

} // namespace

int main() {
    fuse::core::initialize();

    testParseCookManifest();
    testAssetGraphRoundTrip();
    testAssetCookerStub();
    testImportPipelineDryRun();
    testPlanForProject();

    fuse::core::shutdown();
    return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
