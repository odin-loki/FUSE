#include <fuse/core/init.hpp>
#include <fuse/project/asset_cooker.hpp>
#include <fuse/project/asset_graph.hpp>
#include <fuse/project/cook_cache.hpp>
#include <fuse/project/cook_content_hash.hpp>
#include <fuse/project/cook_job_graph.hpp>
#include <fuse/project/cook_manifest.hpp>
#include <fuse/project/import_pipeline.hpp>
#include <fuse/project/loader.hpp>
#include <fuse/project/manifest.hpp>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <thread>

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

void testCookJobGraphStageOrdering() {
    const std::string source = writeTempFile("/tmp/fuse_b79_stage_mesh.obj", "# stage mesh\n");

    fuse::project::CookManifest manifest;
    fuse::project::CookManifestEntry entry;
    entry.kind = fuse::project::CookAssetKind::Mesh;
    entry.source_path = source;
    entry.output_path = "/tmp/fuse_b79_stage_mesh.fusemesh";
    manifest.assets.push_back(entry);

    fuse::project::CookJobGraph graph;
    graph.build_from_manifest(manifest);

    fuse::project::AssetCooker cooker;
    const fuse::project::CookJobGraphExecuteResult result = graph.execute(cooker);

    expectTrue(result.ok, "job graph execute ok");
    expectTrue(result.jobs.size() == 1u, "one job in graph");
    expectTrue(result.jobs[0].stages.size() == 3u, "three stages per job");
    expectTrue(result.jobs[0].stages[0].kind == fuse::project::CookStageKind::Import, "first stage is import");
    expectTrue(result.jobs[0].stages[1].kind == fuse::project::CookStageKind::Process, "second stage is process");
    expectTrue(result.jobs[0].stages[2].kind == fuse::project::CookStageKind::Pack, "third stage is pack");
    expectTrue(result.jobs[0].stages[0].status == fuse::project::CookStageStatus::Ok, "import stage ok");
    expectTrue(result.jobs[0].stages[1].status == fuse::project::CookStageStatus::Ok, "process stage ok");
    expectTrue(result.jobs[0].stages[2].status == fuse::project::CookStageStatus::Ok, "pack stage ok");
}

void testCookJobGraphFailedStageShortCircuit() {
    fuse::project::CookManifest manifest;
    fuse::project::CookManifestEntry entry;
    entry.kind = fuse::project::CookAssetKind::Texture;
    entry.source_path = "/tmp/fuse_b79_missing_texture.png";
    entry.output_path = "/tmp/fuse_b79_missing_texture.fusetex";
    manifest.assets.push_back(entry);

    fuse::project::CookJobGraph graph;
    graph.build_from_manifest(manifest);

    fuse::project::AssetCooker cooker;
    const fuse::project::CookJobGraphExecuteResult result = graph.execute(cooker);

    expectTrue(!result.ok, "missing source fails graph execute");
    expectTrue(!result.failed_job_id.empty(), "failure reports job id");
    expectTrue(result.failed_stage == fuse::project::CookStageKind::Import, "import stage failed");
    expectTrue(result.jobs[0].stages[0].status == fuse::project::CookStageStatus::Failed, "import marked failed");
    expectTrue(result.jobs[0].stages[1].status == fuse::project::CookStageStatus::Skipped, "process short-circuited");
    expectTrue(result.jobs[0].stages[2].status == fuse::project::CookStageStatus::Skipped, "pack short-circuited");
}

void testCookJobGraphDependencyEdgesAndOrder() {
    const std::string sourceA = writeTempFile("/tmp/fuse_b79_dep_mesh_a.obj", "# mesh a\n");
    const std::string sourceB = writeTempFile("/tmp/fuse_b79_dep_mesh_b.obj", "# mesh b\n");

    fuse::project::CookManifest manifest;

    fuse::project::CookManifestEntry entryA;
    entryA.kind = fuse::project::CookAssetKind::Mesh;
    entryA.source_path = sourceA;
    entryA.output_path = "/tmp/fuse_b79_dep_mesh_a.fusemesh";
    manifest.assets.push_back(entryA);

    fuse::project::CookManifestEntry entryB;
    entryB.kind = fuse::project::CookAssetKind::Mesh;
    entryB.source_path = sourceB;
    entryB.output_path = "/tmp/fuse_b79_dep_mesh_b.fusemesh";
    entryB.dependencies.push_back(entryA.output_path);
    manifest.assets.push_back(entryB);

    fuse::project::CookJobGraph graph;
    graph.build_from_manifest(manifest);

    expectTrue(graph.edges().size() >= 1u, "dependency edge recorded");
    expectTrue(graph.edges()[0].from_job_id == entryA.output_path, "edge from producer job");
    expectTrue(graph.edges()[0].to_job_id == entryB.output_path, "edge to dependent job");

    fuse::project::AssetCooker cooker;
    const fuse::project::CookJobGraphExecuteResult result = graph.execute(cooker);

    expectTrue(result.ok, "both jobs succeed");
    expectTrue(result.execution_order.size() == 2u, "two jobs ordered");
    expectTrue(result.execution_order[0] == entryA.output_path, "producer runs first");
    expectTrue(result.execution_order[1] == entryB.output_path, "dependent runs second");
}

void testCookJobGraphDependencyShortCircuit() {
    const std::string sourceB = writeTempFile("/tmp/fuse_b79_dep_mesh_b_skip.obj", "# mesh b\n");

    fuse::project::CookManifest manifest;

    fuse::project::CookManifestEntry entryA;
    entryA.kind = fuse::project::CookAssetKind::Mesh;
    entryA.source_path = "/tmp/fuse_b79_missing_dep_mesh_a.obj";
    entryA.output_path = "/tmp/fuse_b79_dep_mesh_a_skip.fusemesh";
    manifest.assets.push_back(entryA);

    fuse::project::CookManifestEntry entryB;
    entryB.kind = fuse::project::CookAssetKind::Mesh;
    entryB.source_path = sourceB;
    entryB.output_path = "/tmp/fuse_b79_dep_mesh_b_skip.fusemesh";
    entryB.dependencies.push_back(entryA.output_path);
    manifest.assets.push_back(entryB);

    fuse::project::CookJobGraph graph;
    graph.build_from_manifest(manifest);

    fuse::project::AssetCooker cooker;
    const fuse::project::CookJobGraphExecuteResult result = graph.execute(cooker);

    expectTrue(!result.ok, "upstream failure fails batch");
    expectTrue(!result.jobs[0].ok, "producer job failed");
    expectTrue(result.jobs[1].skipped, "dependent skipped after upstream failure");

    const fuse::project::CookJob& dependent = result.jobs[1];
    expectTrue(dependent.stages[0].status == fuse::project::CookStageStatus::Skipped, "dependent import skipped");
    expectTrue(dependent.stages[1].status == fuse::project::CookStageStatus::Skipped, "dependent process skipped");
    expectTrue(dependent.stages[2].status == fuse::project::CookStageStatus::Skipped, "dependent pack skipped");
    expectTrue(dependent.skip_note.find("dependency failed") != std::string::npos, "skip note reports dependency");
}

void testCookManifestUsesJobGraph() {
    const std::string source = writeTempFile("/tmp/fuse_b79_graph_mesh.obj", "# graph mesh\n");

    fuse::project::CookManifest manifest;
    fuse::project::CookManifestEntry entry;
    entry.kind = fuse::project::CookAssetKind::Mesh;
    entry.source_path = source;
    entry.output_path = "/tmp/fuse_b79_graph_mesh.fusemesh";
    manifest.assets.push_back(entry);

    fuse::project::AssetCooker cooker;
    const fuse::project::CookJobGraphExecuteResult graphResult = cooker.cook_manifest_graph(manifest);
    const fuse::project::CookBatchResult batchResult = cooker.cook_manifest(manifest);

    expectTrue(graphResult.ok, "cook_manifest_graph ok");
    expectTrue(batchResult.ok, "cook_manifest ok via job graph");
    expectTrue(batchResult.records.size() == 1u, "batch record emitted");
    expectTrue(batchResult.records[0].note.find("import:ok") != std::string::npos, "stage summary in record note");
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

void testContentHashDeterministic() {
    const std::string source = writeTempFile("/tmp/fuse_b79_hash_mesh.obj", "# hash mesh\n");

    fuse::project::MeshImportDesc desc;
    desc.input_path = source;
    desc.output_path = "/tmp/fuse_b79_hash_mesh.fusemesh";

    const fuse::u64 hash_a = fuse::project::hash_mesh_import(desc);
    const fuse::u64 hash_b = fuse::project::hash_mesh_import(desc);
    expectTrue(hash_a == hash_b, "mesh content hash is deterministic");
    expectTrue(hash_a != 0, "mesh content hash is non-zero");

    const fuse::u64 file_hash = fuse::project::hash_file_content(source);
    expectTrue(file_hash != 0, "file content hash is non-zero");
    expectTrue(fuse::project::hash_file_content("/tmp/fuse_b79_missing.obj") == 0, "missing file hashes to zero");
}

void testContentHashDescSensitivity() {
    const std::string source = writeTempFile("/tmp/fuse_b79_hash_tex.png", "PNG\n");

    fuse::project::TextureImportDesc desc_a;
    desc_a.input_path = source;
    desc_a.output_path = "/tmp/fuse_b79_hash_tex.fusetex";

    fuse::project::TextureImportDesc desc_b = desc_a;
    desc_b.is_normal_map = true;

    const fuse::u64 hash_a = fuse::project::hash_texture_import(desc_a);
    const fuse::u64 hash_b = fuse::project::hash_texture_import(desc_b);
    expectTrue(hash_a != hash_b, "descriptor changes alter content hash");
}

void testCookCacheHitMiss() {
    const std::string source = writeTempFile("/tmp/fuse_b79_cache_mesh.obj", "# cache mesh\n");

    fuse::project::MeshImportDesc desc;
    desc.input_path = source;
    desc.output_path = "/tmp/fuse_b79_cache_mesh.fusemesh";

    fuse::project::AssetCooker cooker;
    const fuse::project::CookRecord first = cooker.cook_mesh(desc);
    expectTrue(first.ok, "first cook ok");
    expectTrue(!first.cache_hit, "first cook is cache miss");
    expectTrue(first.note.find("cache miss") != std::string::npos, "first cook note reports miss");
    expectTrue(cooker.cache().stats().misses == 1u, "one cache miss recorded");
    expectTrue(cooker.cache().stats().hits == 0u, "no cache hits yet");

    const fuse::project::CookRecord second = cooker.cook_mesh(desc);
    expectTrue(second.ok, "second cook ok");
    expectTrue(second.cache_hit, "second cook is cache hit");
    expectTrue(second.content_hash == first.content_hash, "content hash stable across cooks");
    expectTrue(cooker.cache().stats().hits == 1u, "one cache hit recorded");
    expectTrue(cooker.cache().entry_count() == 1u, "one cache entry stored");
}

void testCookCacheInvalidation() {
    const std::string source = writeTempFile("/tmp/fuse_b79_inval_mesh.obj", "# invalidation mesh\n");

    fuse::project::MeshImportDesc desc;
    desc.input_path = source;
    desc.output_path = "/tmp/fuse_b79_inval_mesh.fusemesh";

    fuse::project::AssetCooker cooker;
    const fuse::project::CookRecord seeded = cooker.cook_mesh(desc);
    expectTrue(seeded.ok, "seed cook ok");
    expectTrue(cooker.cache().entry_count() == 1u, "cache seeded");

    expectTrue(cooker.cache().invalidate(seeded.content_hash), "hash invalidation removes entry");
    expectTrue(cooker.cache().entry_count() == 0u, "cache empty after hash invalidation");
    expectTrue(cooker.cache().stats().invalidations >= 1u, "invalidation counted");

    const fuse::project::CookRecord remiss = cooker.cook_mesh(desc);
    expectTrue(remiss.ok, "post-invalidation cook ok");
    expectTrue(!remiss.cache_hit, "post-invalidation cook misses again");

    const fuse::u64 content_hash = remiss.content_hash;
    const fuse::project::CookRecord hit = cooker.cook_mesh(desc);
    expectTrue(hit.cache_hit, "cache repopulated after miss");

    expectTrue(cooker.cache().invalidate_source(source) == 1u, "source invalidation removes entries");
    expectTrue(cooker.cache().lookup(content_hash) == fuse::project::CookCacheLookup::Miss, "lookup misses after source invalidation");

    cooker.cook_mesh(desc);
    cooker.cache().invalidate_all();
    expectTrue(cooker.cache().entry_count() == 0u, "invalidate_all clears cache");
}

void testCookCacheRoundTrip() {
    const std::string source = writeTempFile("/tmp/fuse_b79_cache_persist.obj", "# persist mesh\n");

    fuse::project::MeshImportDesc desc;
    desc.input_path = source;
    desc.output_path = "/tmp/fuse_b79_cache_persist.fusemesh";

    fuse::project::AssetCooker cooker;
    cooker.cook_mesh(desc);

    const std::string cachePath = "/tmp/fuse_b79_cook_cache.json";
    expectTrue(cooker.cache().save(cachePath), "cook cache saves");

    fuse::project::CookCache loaded;
    expectTrue(loaded.load(cachePath), "cook cache loads");
    expectTrue(loaded.entry_count() == 1u, "one cache entry round-tripped");
}

void testCookDirtyInvalidatesCache() {
    const std::string source = writeTempFile("/tmp/fuse_b79_dirty_mesh.obj", "# dirty mesh\n");

    fuse::project::AssetGraph graph;
    graph.add_asset("/tmp/fuse_b79_dirty_mesh.fusemesh", source);

    fuse::project::MeshImportDesc desc;
    desc.input_path = source;
    desc.output_path = "/tmp/fuse_b79_dirty_mesh.fusemesh";

    fuse::project::AssetCooker cooker;
    const fuse::project::CookRecord seeded = cooker.cook_mesh(desc);
    expectTrue(seeded.ok, "seed dirty-path cook ok");
    expectTrue(cooker.cache().entry_count() == 1u, "cache seeded for dirty test");

    // Ensure filesystem mtime advances before rewriting the source.
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    writeTempFile(source, "# dirty mesh updated\n");
    graph.scan_for_changes();
    expectTrue(!graph.dirty_assets().empty(), "asset marked dirty after source change");

    const fuse::project::CookBatchResult reimport = cooker.cook_dirty(graph, "/tmp/demo");
    expectTrue(reimport.ok, "dirty reimport ok");
    expectTrue(cooker.cache().entry_count() == 0u, "dirty reimport invalidates cache");
    expectTrue(cooker.cache().stats().invalidations >= 1u, "dirty invalidation counted");
}

} // namespace

int main() {
    fuse::core::initialize();

    testParseCookManifest();
    testAssetGraphRoundTrip();
    testAssetCookerStub();
    testCookJobGraphStageOrdering();
    testCookJobGraphFailedStageShortCircuit();
    testCookJobGraphDependencyEdgesAndOrder();
    testCookJobGraphDependencyShortCircuit();
    testCookManifestUsesJobGraph();
    testImportPipelineDryRun();
    testPlanForProject();
    testContentHashDeterministic();
    testContentHashDescSensitivity();
    testCookCacheHitMiss();
    testCookCacheInvalidation();
    testCookCacheRoundTrip();
    testCookDirtyInvalidatesCache();

    fuse::core::shutdown();
    return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
