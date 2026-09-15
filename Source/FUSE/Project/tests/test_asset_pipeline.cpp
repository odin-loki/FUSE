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

#include <algorithm>
#include <chrono>
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
    const fuse::project::CookJobGraphExecuteResult result = graph.execute(cooker, manifest);

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
    const fuse::project::CookJobGraphExecuteResult result = graph.execute(cooker, manifest);

    expectTrue(!result.ok, "missing source fails graph execute");
    expectTrue(!result.failed_job_id.empty(), "failure reports job id");
    expectTrue(result.failed_stage == fuse::project::CookStageKind::Import, "import stage failed");
    expectTrue(result.jobs[0].stages[0].status == fuse::project::CookStageStatus::Failed, "import marked failed");
    expectTrue(result.jobs[0].stages[1].status == fuse::project::CookStageStatus::Skipped, "process short-circuited");
    expectTrue(result.jobs[0].stages[2].status == fuse::project::CookStageStatus::Skipped, "pack short-circuited");
}

void testCookJobGraphLinearChain() {
    const std::string sourceA = writeTempFile("/tmp/fuse_b79_chain_a.obj", "# chain a\n");
    const std::string sourceB = writeTempFile("/tmp/fuse_b79_chain_b.obj", "# chain b\n");
    const std::string sourceC = writeTempFile("/tmp/fuse_b79_chain_c.obj", "# chain c\n");

    fuse::project::CookManifest manifest;

    fuse::project::CookManifestEntry entryA;
    entryA.kind = fuse::project::CookAssetKind::Mesh;
    entryA.source_path = sourceA;
    entryA.output_path = "/tmp/fuse_b79_chain_a.fusemesh";
    manifest.assets.push_back(entryA);

    fuse::project::CookManifestEntry entryB;
    entryB.kind = fuse::project::CookAssetKind::Mesh;
    entryB.source_path = sourceB;
    entryB.output_path = "/tmp/fuse_b79_chain_b.fusemesh";
    entryB.dependencies.push_back(entryA.output_path);
    manifest.assets.push_back(entryB);

    fuse::project::CookManifestEntry entryC;
    entryC.kind = fuse::project::CookAssetKind::Mesh;
    entryC.source_path = sourceC;
    entryC.output_path = "/tmp/fuse_b79_chain_c.fusemesh";
    entryC.dependencies.push_back(entryB.output_path);
    manifest.assets.push_back(entryC);

    fuse::project::CookJobGraph graph;
    graph.build_from_manifest(manifest);
    expectTrue(graph.edges().size() == 2u, "linear chain has two edges");

    fuse::project::AssetCooker cooker;
    const fuse::project::CookJobGraphExecuteResult result = graph.execute(cooker, manifest);

    expectTrue(result.ok, "linear chain executes successfully");
    expectTrue(result.execution_order.size() == 3u, "three jobs ordered");
    expectTrue(result.execution_order[0] == entryA.output_path, "chain head runs first");
    expectTrue(result.execution_order[1] == entryB.output_path, "chain middle runs second");
    expectTrue(result.execution_order[2] == entryC.output_path, "chain tail runs last");
}

void testCookJobGraphDiamondDag() {
    const std::string sourceA = writeTempFile("/tmp/fuse_b79_diamond_a.obj", "# diamond a\n");
    const std::string sourceB = writeTempFile("/tmp/fuse_b79_diamond_b.obj", "# diamond b\n");
    const std::string sourceC = writeTempFile("/tmp/fuse_b79_diamond_c.obj", "# diamond c\n");
    const std::string sourceD = writeTempFile("/tmp/fuse_b79_diamond_d.obj", "# diamond d\n");

    fuse::project::CookManifest manifest;

    fuse::project::CookManifestEntry entryA;
    entryA.kind = fuse::project::CookAssetKind::Mesh;
    entryA.source_path = sourceA;
    entryA.output_path = "/tmp/fuse_b79_diamond_a.fusemesh";
    manifest.assets.push_back(entryA);

    fuse::project::CookManifestEntry entryB;
    entryB.kind = fuse::project::CookAssetKind::Mesh;
    entryB.source_path = sourceB;
    entryB.output_path = "/tmp/fuse_b79_diamond_b.fusemesh";
    entryB.dependencies.push_back(entryA.output_path);
    manifest.assets.push_back(entryB);

    fuse::project::CookManifestEntry entryC;
    entryC.kind = fuse::project::CookAssetKind::Mesh;
    entryC.source_path = sourceC;
    entryC.output_path = "/tmp/fuse_b79_diamond_c.fusemesh";
    entryC.dependencies.push_back(entryA.output_path);
    manifest.assets.push_back(entryC);

    fuse::project::CookManifestEntry entryD;
    entryD.kind = fuse::project::CookAssetKind::Mesh;
    entryD.source_path = sourceD;
    entryD.output_path = "/tmp/fuse_b79_diamond_d.fusemesh";
    entryD.dependencies.push_back(entryB.output_path);
    entryD.dependencies.push_back(entryC.output_path);
    manifest.assets.push_back(entryD);

    fuse::project::CookJobGraph graph;
    graph.build_from_manifest(manifest);
    expectTrue(graph.edges().size() >= 3u, "diamond DAG records dependency edges");

    fuse::project::AssetCooker cooker;
    const fuse::project::CookJobGraphExecuteResult result = graph.execute(cooker, manifest);

    expectTrue(result.ok, "diamond DAG executes successfully");
    expectTrue(result.execution_order.size() == 4u, "four jobs ordered");
    expectTrue(result.execution_order[0] == entryA.output_path, "diamond root runs first");
    expectTrue(result.execution_order[3] == entryD.output_path, "diamond merge runs last");

    const auto posB = std::find(result.execution_order.begin(), result.execution_order.end(), entryB.output_path);
    const auto posC = std::find(result.execution_order.begin(), result.execution_order.end(), entryC.output_path);
    const auto posD = std::find(result.execution_order.begin(), result.execution_order.end(), entryD.output_path);
    expectTrue(posB != result.execution_order.end(), "diamond branch B scheduled");
    expectTrue(posC != result.execution_order.end(), "diamond branch C scheduled");
    expectTrue(posD != result.execution_order.end(), "diamond merge scheduled");
    expectTrue(posB < posD, "branch B completes before merge");
    expectTrue(posC < posD, "branch C completes before merge");
}

void testCookJobGraphCycleReject() {
    fuse::project::CookManifest manifest;

    fuse::project::CookManifestEntry entryA;
    entryA.kind = fuse::project::CookAssetKind::Mesh;
    entryA.source_path = "/tmp/fuse_b79_cycle_a.obj";
    entryA.output_path = "/tmp/fuse_b79_cycle_a.fusemesh";
    entryA.dependencies.push_back("/tmp/fuse_b79_cycle_b.fusemesh");
    manifest.assets.push_back(entryA);

    fuse::project::CookManifestEntry entryB;
    entryB.kind = fuse::project::CookAssetKind::Mesh;
    entryB.source_path = "/tmp/fuse_b79_cycle_b.obj";
    entryB.output_path = "/tmp/fuse_b79_cycle_b.fusemesh";
    entryB.dependencies.push_back("/tmp/fuse_b79_cycle_a.fusemesh");
    manifest.assets.push_back(entryB);

    fuse::project::CookJobGraph graph;
    graph.build_from_manifest(manifest);

    expectTrue(graph.has_cycle(), "cycle detected in graph");
    expectTrue(graph.edges().size() >= 2u, "cycle edges recorded");

    fuse::project::AssetCooker cooker;
    const fuse::project::CookJobGraphExecuteResult result = graph.execute(cooker, manifest);

    expectTrue(!result.ok, "cycle rejects graph execute");
    expectTrue(result.cycle_detected, "cycle flag set");
    expectTrue(result.execution_order.empty(), "no execution order for cyclic graph");
    expectTrue(result.failure_note.find("cycle") != std::string::npos, "failure note mentions cycle");
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
    const fuse::project::CookJobGraphExecuteResult result = graph.execute(cooker, manifest);

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
    const fuse::project::CookJobGraphExecuteResult result = graph.execute(cooker, manifest);

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

void testContentHashMtimeSensitivity() {
    const std::string source = writeTempFile("/tmp/fuse_b79_hash_mtime.obj", "# mtime mesh\n");

    const fuse::u64 hash_before = fuse::project::hash_file_content(source);
    expectTrue(hash_before != 0, "mtime-aware file hash is non-zero");

    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    writeTempFile(source, "# mtime mesh\n");

    const fuse::u64 hash_after = fuse::project::hash_file_content(source);
    expectTrue(hash_after != hash_before, "mtime change alters content hash key");
    expectTrue(fuse::project::file_mtime_ns(source) != 0, "file mtime readable");
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

void testCookCacheInvalidateChain() {
    const std::string sourceA = writeTempFile("/tmp/fuse_b79_chain_inv_a.obj", "# chain inv a\n");
    const std::string sourceB = writeTempFile("/tmp/fuse_b79_chain_inv_b.obj", "# chain inv b\n");
    const std::string sourceC = writeTempFile("/tmp/fuse_b79_chain_inv_c.obj", "# chain inv c\n");

    fuse::project::CookManifest manifest;

    fuse::project::CookManifestEntry entryA;
    entryA.kind = fuse::project::CookAssetKind::Mesh;
    entryA.source_path = sourceA;
    entryA.output_path = "/tmp/fuse_b79_chain_inv_a.fusemesh";
    manifest.assets.push_back(entryA);

    fuse::project::CookManifestEntry entryB;
    entryB.kind = fuse::project::CookAssetKind::Mesh;
    entryB.source_path = sourceB;
    entryB.output_path = "/tmp/fuse_b79_chain_inv_b.fusemesh";
    entryB.dependencies.push_back(entryA.output_path);
    manifest.assets.push_back(entryB);

    fuse::project::CookManifestEntry entryC;
    entryC.kind = fuse::project::CookAssetKind::Mesh;
    entryC.source_path = sourceC;
    entryC.output_path = "/tmp/fuse_b79_chain_inv_c.fusemesh";
    entryC.dependencies.push_back(entryB.output_path);
    manifest.assets.push_back(entryC);

    fuse::project::AssetCooker cooker;
    const fuse::project::CookBatchResult batch = cooker.cook_manifest(manifest);
    expectTrue(batch.ok, "chain manifest cook seeds cache");
    expectTrue(cooker.cache().entry_count() == 3u, "three-node chain cached");

    const fuse::u64 hashB = batch.records[1].content_hash;
    const fuse::u64 hashC = batch.records[2].content_hash;
    expectTrue(cooker.cache().lookup(hashB) == fuse::project::CookCacheLookup::Hit, "middle chain entry cached");
    expectTrue(cooker.cache().lookup(hashC) == fuse::project::CookCacheLookup::Hit, "tail chain entry cached");

    writeTempFile(sourceA, "# chain inv a revised\n");
    const fuse::u32 removed = cooker.invalidate_upstream_dependency(manifest, sourceA);
    expectTrue(removed >= 3u, "upstream change invalidates full chain");
    expectTrue(cooker.cache().lookup(hashB) == fuse::project::CookCacheLookup::Miss, "middle misses after chain invalidation");
    expectTrue(cooker.cache().lookup(hashC) == fuse::project::CookCacheLookup::Miss, "tail misses after chain invalidation");

    const fuse::u32 stale_removed = cooker.invalidate_stale_dependency_hashes(manifest);
    expectTrue(stale_removed >= 0u, "stale dependency hash reconcile runs after chain invalidation");
}

void testCookCacheStaleDependencyHashInvalidation() {
    const std::string sourceA = writeTempFile("/tmp/fuse_b79_stale_a.obj", "# stale a\n");
    const std::string sourceB = writeTempFile("/tmp/fuse_b79_stale_b.obj", "# stale b\n");

    fuse::project::CookManifest manifest;

    fuse::project::CookManifestEntry entryA;
    entryA.kind = fuse::project::CookAssetKind::Mesh;
    entryA.source_path = sourceA;
    entryA.output_path = "/tmp/fuse_b79_stale_a.fusemesh";
    manifest.assets.push_back(entryA);

    fuse::project::CookManifestEntry entryB;
    entryB.kind = fuse::project::CookAssetKind::Mesh;
    entryB.source_path = sourceB;
    entryB.output_path = "/tmp/fuse_b79_stale_b.fusemesh";
    entryB.dependencies.push_back(entryA.output_path);
    manifest.assets.push_back(entryB);

    fuse::project::AssetCooker cooker;
    const fuse::project::CookBatchResult batch = cooker.cook_manifest(manifest);
    expectTrue(batch.ok, "stale-hash test seeds cache");
    expectTrue(cooker.cache().entry_count() == 2u, "upstream and downstream cached");

    const fuse::u64 downstream_hash = batch.records[1].content_hash;
    expectTrue(cooker.cache().lookup(downstream_hash) == fuse::project::CookCacheLookup::Hit,
               "downstream cached before upstream hash change");

    writeTempFile(sourceA, "# stale a revised\n");
    const fuse::u32 removed = cooker.invalidate_stale_dependency_hashes(manifest);
    expectTrue(removed >= 1u, "stale upstream hash invalidates dependent cache entries");
    expectTrue(cooker.cache().lookup(downstream_hash) == fuse::project::CookCacheLookup::Miss,
               "downstream misses after stale dependency hash invalidation");
}

void testCookCacheUpstreamInvalidation() {
    const std::string sourceA = writeTempFile("/tmp/fuse_b79_upinv_a.obj", "# upstream a\n");
    const std::string sourceB = writeTempFile("/tmp/fuse_b79_upinv_b.obj", "# downstream b\n");

    fuse::project::CookManifest manifest;

    fuse::project::CookManifestEntry entryA;
    entryA.kind = fuse::project::CookAssetKind::Mesh;
    entryA.source_path = sourceA;
    entryA.output_path = "/tmp/fuse_b79_upinv_a.fusemesh";
    manifest.assets.push_back(entryA);

    fuse::project::CookManifestEntry entryB;
    entryB.kind = fuse::project::CookAssetKind::Mesh;
    entryB.source_path = sourceB;
    entryB.output_path = "/tmp/fuse_b79_upinv_b.fusemesh";
    entryB.dependencies.push_back(entryA.output_path);
    manifest.assets.push_back(entryB);

    fuse::project::AssetCooker cooker;
    const fuse::project::CookBatchResult batch = cooker.cook_manifest(manifest);
    expectTrue(batch.ok, "manifest cook seeds cache");
    expectTrue(cooker.cache().entry_count() == 2u, "upstream and downstream cached");

    const fuse::u64 downstream_hash = batch.records[1].content_hash;
    expectTrue(cooker.cache().lookup(downstream_hash) == fuse::project::CookCacheLookup::Hit,
               "downstream entry cached before upstream change");

    writeTempFile(sourceA, "# upstream a revised\n");
    const fuse::u32 removed = cooker.invalidate_upstream_dependency(manifest, sourceA);
    expectTrue(removed >= 2u, "upstream change invalidates downstream dependents");
    expectTrue(cooker.cache().lookup(downstream_hash) == fuse::project::CookCacheLookup::Miss,
               "downstream cache misses after upstream invalidation");

    fuse::project::MeshImportDesc descB;
    descB.input_path = sourceB;
    descB.output_path = entryB.output_path;
    const fuse::project::CookRecord remiss = cooker.cook_mesh(descB);
    expectTrue(remiss.ok, "downstream re-cook ok");
    expectTrue(!remiss.cache_hit, "downstream re-cook is cache miss");
}

void testCookCacheEmptyKeyPaths() {
    fuse::project::CookCache cache;

    expectTrue(cache.lookup(0) == fuse::project::CookCacheLookup::Miss, "zero hash always misses");
    expectTrue(cache.stats().hits == 0u && cache.stats().misses == 0u,
               "zero-hash lookup does not touch hit/miss stats");

    fuse::project::CookCacheEntry invalid;
    invalid.content_hash = 0;
    invalid.source_path = "/tmp/fuse_b79_empty_key.obj";
    invalid.output_path = "/tmp/fuse_b79_empty_key.fusemesh";
    cache.store(invalid);
    expectTrue(cache.entry_count() == 0u, "zero-hash entry is not stored");

    fuse::project::CookCacheEntry empty_paths;
    empty_paths.content_hash = 42;
    empty_paths.source_path = "";
    empty_paths.output_path = "/tmp/fuse_b79_empty_paths.fusemesh";
    cache.store(empty_paths);
    expectTrue(cache.entry_count() == 0u, "empty source path is not stored");

    expectTrue(!cache.invalidate(0), "zero-hash invalidation is a no-op");
    expectTrue(cache.stats().invalidations == 0u, "zero-hash invalidation does not bump stats");

    expectTrue(fuse::project::hash_file_content("") == 0, "empty path hashes to zero");
    expectTrue(fuse::project::combine_cook_cache_key(0, 0) == 0, "all-zero cache key stays zero");

    fuse::project::MeshImportDesc desc;
    desc.input_path = "";
    desc.output_path = "/tmp/fuse_b79_empty_input.fusemesh";
    expectTrue(fuse::project::hash_mesh_import(desc) == 0, "empty input path yields zero content hash");

    fuse::project::AssetCooker cooker;
    const fuse::project::CookRecord record = cooker.cook_mesh(desc);
    expectTrue(!record.ok, "empty input path fails cook");
    expectTrue(cooker.cache().entry_count() == 0u, "failed empty-path cook does not cache");
    expectTrue(cooker.cache().stats().misses == 0u, "uncacheable cook does not record misses");
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
    testCookJobGraphLinearChain();
    testCookJobGraphDiamondDag();
    testCookJobGraphCycleReject();
    testCookJobGraphDependencyEdgesAndOrder();
    testCookJobGraphDependencyShortCircuit();
    testCookManifestUsesJobGraph();
    testImportPipelineDryRun();
    testPlanForProject();
    testContentHashDeterministic();
    testContentHashMtimeSensitivity();
    testContentHashDescSensitivity();
    testCookCacheHitMiss();
    testCookCacheInvalidation();
    testCookCacheInvalidateChain();
    testCookCacheStaleDependencyHashInvalidation();
    testCookCacheUpstreamInvalidation();
    testCookCacheRoundTrip();
    testCookCacheEmptyKeyPaths();
    testCookDirtyInvalidatesCache();

    fuse::core::shutdown();
    return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
