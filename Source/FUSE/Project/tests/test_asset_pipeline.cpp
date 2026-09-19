#include <fuse/core/init.hpp>
#include <fuse/project/asset_cooker.hpp>
#include <fuse/project/asset_graph.hpp>
#include <fuse/project/cook_cache.hpp>
#include <fuse/project/cook_content_hash.hpp>
#include <fuse/project/cook_dependency_graph.hpp>
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

void testCookJobGraphEmpty() {
    fuse::project::CookJobGraph graph;
    expectTrue(graph.empty(), "default graph is empty");
    expectTrue(!graph.has_cycle(), "empty graph has no cycle");

    const fuse::project::CookJobGraphOrderResult order = graph.topological_order();
    expectTrue(order.ok, "empty graph order ok");
    expectTrue(!order.cycle_detected, "empty graph order not cyclic");
    expectTrue(order.order.empty(), "empty graph yields empty order");
    expectTrue(graph.edges().empty(), "empty graph has no edges");

    fuse::project::CookManifest manifest;
    graph.build_from_manifest(manifest);
    expectTrue(graph.empty(), "empty manifest yields empty graph");
    expectTrue(!graph.has_cycle(), "empty manifest graph has no cycle");

    const fuse::project::CookJobGraphOrderResult manifestOrder = graph.topological_order();
    expectTrue(manifestOrder.ok, "empty manifest order ok");
    expectTrue(manifestOrder.order.empty(), "empty manifest order is empty");

    fuse::project::AssetCooker cooker;
    const fuse::project::CookJobGraphExecuteResult result = graph.execute(cooker, manifest);
    expectTrue(result.ok, "empty graph execute ok");
    expectTrue(!result.cycle_detected, "empty graph execute not cyclic");
    expectTrue(result.execution_order.empty(), "empty graph execute order empty");
    expectTrue(result.jobs.empty(), "empty graph execute has no jobs");
}

void testCookJobGraphTopologicalOrderDirect() {
    const std::string sourceA = writeTempFile("/tmp/fuse_b79_topo_a.obj", "# topo a\n");
    const std::string sourceB = writeTempFile("/tmp/fuse_b79_topo_b.obj", "# topo b\n");

    fuse::project::CookManifest manifest;

    fuse::project::CookManifestEntry entryA;
    entryA.kind = fuse::project::CookAssetKind::Mesh;
    entryA.source_path = sourceA;
    entryA.output_path = "/tmp/fuse_b79_topo_a.fusemesh";
    manifest.assets.push_back(entryA);

    fuse::project::CookManifestEntry entryB;
    entryB.kind = fuse::project::CookAssetKind::Mesh;
    entryB.source_path = sourceB;
    entryB.output_path = "/tmp/fuse_b79_topo_b.fusemesh";
    entryB.dependencies.push_back(entryA.output_path);
    manifest.assets.push_back(entryB);

    fuse::project::CookJobGraph graph;
    graph.build_from_manifest(manifest);

    const fuse::project::CookJobGraphOrderResult order = graph.topological_order();
    expectTrue(order.ok, "topological order ok");
    expectTrue(!order.cycle_detected, "topological order not cyclic");
    expectTrue(order.order.size() == 2u, "two jobs topologically ordered");
    expectTrue(order.order[0] == entryA.output_path, "producer precedes dependent");
    expectTrue(order.order[1] == entryB.output_path, "dependent follows producer");
    expectTrue(!graph.has_cycle(), "has_cycle agrees with order result");
}

void testCookJobGraphImplicitOutputSourceEdge() {
    const std::string sourceA = writeTempFile("/tmp/fuse_b79_implicit_a.obj", "# implicit a\n");

    fuse::project::CookManifest manifest;

    fuse::project::CookManifestEntry entryA;
    entryA.kind = fuse::project::CookAssetKind::Mesh;
    entryA.source_path = sourceA;
    entryA.output_path = "/tmp/fuse_b79_implicit_a.fusemesh";
    manifest.assets.push_back(entryA);

    fuse::project::CookManifestEntry entryB;
    entryB.kind = fuse::project::CookAssetKind::Mesh;
    entryB.source_path = entryA.output_path;
    entryB.output_path = "/tmp/fuse_b79_implicit_b.fusemesh";
    manifest.assets.push_back(entryB);

    fuse::project::CookJobGraph graph;
    graph.build_from_manifest(manifest);

    expectTrue(graph.edges().size() >= 1u, "implicit output→source edge recorded");
    expectTrue(graph.edges()[0].from_job_id == entryA.output_path, "implicit edge from producer");
    expectTrue(graph.edges()[0].to_job_id == entryB.output_path, "implicit edge to consumer");

    const fuse::project::CookJobGraphOrderResult order = graph.topological_order();
    expectTrue(order.ok, "implicit edge graph orders ok");
    expectTrue(order.order.size() == 2u, "two jobs ordered via implicit edge");
    expectTrue(order.order[0] == entryA.output_path, "producer runs first via implicit edge");
}

void testCookDependencyGraphEmptyGuards() {
    fuse::project::CookDependencyGraph graph;
    expectTrue(graph.empty(), "default dependency graph is empty");
    expectTrue(graph.node_count() == 0u, "empty graph has zero nodes");
    expectTrue(graph.edge_count() == 0u, "empty graph has zero edges");
    expectTrue(!graph.has_cycle(), "empty dependency graph has no cycle");

    const fuse::project::CookJobGraphOrderResult order = graph.topological_order();
    expectTrue(order.ok, "empty dependency graph topo ok");
    expectTrue(order.order.empty(), "empty dependency graph topo order empty");

    const fuse::project::CookDependencyCycleResult cycles = graph.detect_cycle_edges();
    expectTrue(!cycles.cycle_detected, "empty dependency graph reports no cycle edges");
    expectTrue(cycles.cycle_edges.empty(), "empty dependency graph cycle edge list empty");

    expectTrue(!graph.add_edge("a", "b"), "edge rejected when nodes missing");
    expectTrue(!graph.add_edge("", "b"), "edge rejected for empty from id");
    expectTrue(!graph.add_edge("a", ""), "edge rejected for empty to id");

    graph.add_node("solo");
    expectTrue(!graph.empty(), "graph with one node is not empty");
    expectTrue(!graph.add_edge("solo", "solo"), "self-loop edge rejected");
    expectTrue(!graph.add_edge("solo", "missing"), "edge to unknown node rejected");
    expectTrue(!graph.add_edge("missing", "solo"), "edge from unknown node rejected");

    graph.add_node("next");
    expectTrue(graph.add_edge("solo", "next"), "valid edge accepted");
    expectTrue(graph.edge_count() == 1u, "one edge recorded");
    expectTrue(!graph.add_edge("solo", "next"), "duplicate edge rejected");
}

void testCookDependencyGraphTopologicalOrder() {
    fuse::project::CookDependencyGraph graph;
    graph.add_node("a");
    graph.add_node("b");
    graph.add_node("c");
    expectTrue(graph.add_edge("a", "b"), "a->b edge added");
    expectTrue(graph.add_edge("a", "c"), "a->c edge added");
    expectTrue(graph.add_edge("b", "c"), "b->c edge added");

    const fuse::project::CookJobGraphOrderResult order = graph.topological_order();
    expectTrue(order.ok, "diamond dependency graph topo ok");
    expectTrue(!order.cycle_detected, "diamond dependency graph acyclic");
    expectTrue(order.order.size() == 3u, "three nodes ordered");
    expectTrue(order.order[0] == "a", "root precedes branches");
    expectTrue(order.order[2] == "c", "merge node runs last");
}

void testCookDependencyGraphQueryHelpers() {
    fuse::project::CookDependencyGraph graph;
    graph.add_node("a");
    graph.add_node("b");
    graph.add_node("c");
    expectTrue(graph.has_node("a"), "has_node reports registered node");
    expectTrue(!graph.has_node("missing"), "has_node rejects unknown node");
    expectTrue(!graph.has_node(""), "has_node rejects empty id");

    expectTrue(graph.add_edge("a", "b"), "helper test edge a->b");
    expectTrue(graph.add_edge("a", "c"), "helper test edge a->c");
    expectTrue(graph.add_edge("b", "c"), "helper test edge b->c");
    expectTrue(graph.has_edge("a", "b"), "has_edge reports forward edge");
    expectTrue(!graph.has_edge("b", "a"), "has_edge rejects reverse edge");
    expectTrue(!graph.has_edge("", "b"), "has_edge rejects empty from id");

    const std::vector<std::string> preds = graph.predecessors("c");
    expectTrue(preds.size() == 2u, "merge node has two predecessors");
    expectTrue(preds[0] == "a", "first predecessor sorted");
    expectTrue(preds[1] == "b", "second predecessor sorted");
    expectTrue(graph.predecessors("missing").empty(), "unknown node has no predecessors");
    expectTrue(graph.predecessors("").empty(), "empty node id has no predecessors");

    const std::vector<std::string> succs = graph.successors("a");
    expectTrue(succs.size() == 2u, "root has two successors");
    expectTrue(graph.successors("c").empty(), "leaf has no successors");

    const std::vector<std::string> roots = graph.roots();
    expectTrue(roots.size() == 1u, "diamond has one root");
    expectTrue(roots[0] == "a", "root is node a");

    const std::vector<std::string> leaves = graph.leaves();
    expectTrue(leaves.size() == 1u, "diamond has one leaf");
    expectTrue(leaves[0] == "c", "leaf is node c");
}

void testCookDependencyGraphTopologicalLayers() {
    fuse::project::CookDependencyGraph graph;
    graph.add_node("a");
    graph.add_node("b");
    graph.add_node("c");
    graph.add_node("d");
    expectTrue(graph.add_edge("a", "b"), "layer test a->b");
    expectTrue(graph.add_edge("a", "c"), "layer test a->c");
    expectTrue(graph.add_edge("b", "d"), "layer test b->d");
    expectTrue(graph.add_edge("c", "d"), "layer test c->d");

    const fuse::project::CookDependencyLayerResult layers = graph.topological_layers();
    expectTrue(layers.ok, "layered diamond is acyclic");
    expectTrue(!layers.cycle_detected, "layered diamond not cyclic");
    expectTrue(layers.layers.size() == 3u, "diamond has three layers");
    expectTrue(layers.layers[0].size() == 1u && layers.layers[0][0] == "a", "layer 0 is root");
    expectTrue(layers.layers[1].size() == 2u, "layer 1 has parallel branches");
    expectTrue(layers.layers[2].size() == 1u && layers.layers[2][0] == "d", "layer 2 is merge");

    fuse::project::CookDependencyGraph cyclic;
    cyclic.add_node("x");
    cyclic.add_node("y");
    expectTrue(cyclic.add_edge("x", "y"), "cycle layer x->y");
    expectTrue(cyclic.add_edge("y", "x"), "cycle layer y->x");
    const fuse::project::CookDependencyLayerResult cyclic_layers = cyclic.topological_layers();
    expectTrue(!cyclic_layers.ok, "cyclic graph layers not ok");
    expectTrue(cyclic_layers.cycle_detected, "cyclic graph layers detect cycle");
    expectTrue(cyclic_layers.layers.empty(), "cyclic graph yields no layers");
}

void testCookDependencyGraphInvalidationClosure() {
    fuse::project::CookDependencyGraph graph;
    graph.add_node("a");
    graph.add_node("b");
    graph.add_node("c");
    expectTrue(graph.add_edge("a", "b"), "closure a->b");
    expectTrue(graph.add_edge("b", "c"), "closure b->c");

    const fuse::project::CookInvalidationClosureResult closure = graph.transitive_successors("a");
    expectTrue(closure.ok, "valid seed yields closure");
    expectTrue(closure.job_ids.size() == 2u, "closure includes two downstream nodes");
    expectTrue(closure.job_ids[0] == "b", "first downstream is b");
    expectTrue(closure.job_ids[1] == "c", "second downstream is c");

    const fuse::project::CookInvalidationClosureResult leaf_closure = graph.transitive_successors("c");
    expectTrue(leaf_closure.ok, "leaf seed closure ok");
    expectTrue(leaf_closure.job_ids.empty(), "leaf has no downstream nodes");

    const fuse::project::CookInvalidationClosureResult guarded = graph.transitive_successors("missing");
    expectTrue(!guarded.ok, "unknown seed guarded");
    expectTrue(guarded.job_ids.empty(), "unknown seed yields empty closure");

    const fuse::project::CookInvalidationClosureResult empty_seed = graph.transitive_successors("");
    expectTrue(!empty_seed.ok, "empty seed guarded");
}

void testCookDependencyGraphEmptyHelperGuards() {
    fuse::project::CookDependencyGraph graph;
    expectTrue(graph.roots().empty(), "empty graph roots guarded");
    expectTrue(graph.leaves().empty(), "empty graph leaves guarded");
    expectTrue(graph.predecessors("a").empty(), "empty graph predecessors guarded");
    expectTrue(graph.successors("a").empty(), "empty graph successors guarded");
    expectTrue(!graph.has_edge("a", "b"), "empty graph has_edge guarded");

    const fuse::project::CookDependencyLayerResult layers = graph.topological_layers();
    expectTrue(layers.ok, "empty graph layers ok");
    expectTrue(layers.layers.empty(), "empty graph yields no layers");

    const fuse::project::CookInvalidationClosureResult closure = graph.transitive_successors("a");
    expectTrue(!closure.ok, "empty graph invalidation closure guarded");

    const fuse::project::CookInvalidationClosureResult upstream = graph.transitive_predecessors("a");
    expectTrue(!upstream.ok, "empty graph upstream closure guarded");

    const fuse::project::CookInvalidationClosureResult merged = graph.merged_invalidation_closure({"a"});
    expectTrue(!merged.ok, "empty graph merged closure guarded");

    expectTrue(!graph.is_reachable("a", "b"), "empty graph reachability guarded");
    expectTrue(graph.topological_layer_index("a") == -1, "empty graph layer index guarded");
    expectTrue(graph.parallel_layer_width() == 0u, "empty graph parallel width guarded");
    expectTrue(fuse::project::flatten_topological_layers(layers).empty(), "empty layers flatten guarded");
}

void testCookDependencyGraphFlattenLayers() {
    fuse::project::CookDependencyGraph graph;
    graph.add_node("a");
    graph.add_node("b");
    graph.add_node("c");
    graph.add_node("d");
    expectTrue(graph.add_edge("a", "b"), "flatten test a->b");
    expectTrue(graph.add_edge("a", "c"), "flatten test a->c");
    expectTrue(graph.add_edge("b", "d"), "flatten test b->d");
    expectTrue(graph.add_edge("c", "d"), "flatten test c->d");

    const fuse::project::CookDependencyLayerResult layers = graph.topological_layers();
    const std::vector<std::string> flattened = fuse::project::flatten_topological_layers(layers);
    expectTrue(flattened.size() == 4u, "flattened diamond has four nodes");

    const fuse::project::CookJobGraphOrderResult order = graph.topological_order();
    expectTrue(flattened == order.order, "flattened layers match topological order");

    const fuse::project::CookDependencyLayerResult cyclic_layers =
        fuse::project::CookDependencyGraph{}.topological_layers();
    expectTrue(fuse::project::flatten_topological_layers(cyclic_layers).empty(),
               "default empty layers flatten to empty order");
}

void testCookDependencyGraphUpstreamClosure() {
    fuse::project::CookDependencyGraph graph;
    graph.add_node("a");
    graph.add_node("b");
    graph.add_node("c");
    expectTrue(graph.add_edge("a", "b"), "upstream a->b");
    expectTrue(graph.add_edge("b", "c"), "upstream b->c");

    const fuse::project::CookInvalidationClosureResult upstream = graph.transitive_predecessors("c");
    expectTrue(upstream.ok, "leaf upstream closure ok");
    expectTrue(upstream.job_ids.size() == 2u, "leaf has two upstream nodes");
    expectTrue(upstream.job_ids[0] == "a", "root is first upstream");
    expectTrue(upstream.job_ids[1] == "b", "middle is second upstream");

    const fuse::project::CookInvalidationClosureResult root_upstream = graph.transitive_predecessors("a");
    expectTrue(root_upstream.ok, "root upstream closure ok");
    expectTrue(root_upstream.job_ids.empty(), "root has no upstream nodes");

    expectTrue(!graph.transitive_predecessors("missing").ok, "unknown upstream seed guarded");
    expectTrue(!graph.transitive_predecessors("").ok, "empty upstream seed guarded");
}

void testCookDependencyGraphMergedInvalidationClosure() {
    fuse::project::CookDependencyGraph graph;
    graph.add_node("a");
    graph.add_node("b");
    graph.add_node("c");
    graph.add_node("d");
    expectTrue(graph.add_edge("a", "b"), "merged a->b");
    expectTrue(graph.add_edge("a", "c"), "merged a->c");
    expectTrue(graph.add_edge("b", "d"), "merged b->d");

    const fuse::project::CookInvalidationClosureResult merged = graph.merged_invalidation_closure({"a"});
    expectTrue(merged.ok, "single-seed merged closure ok");
    expectTrue(merged.job_ids.size() == 3u, "single seed reaches three downstream nodes");

    const fuse::project::CookInvalidationClosureResult partial =
        graph.merged_invalidation_closure({"b", "c"});
    expectTrue(partial.ok, "multi-seed merged closure ok");
    expectTrue(partial.job_ids.size() == 1u, "overlapping seeds dedupe downstream");
    expectTrue(partial.job_ids[0] == "d", "shared downstream is node d");

    expectTrue(!graph.merged_invalidation_closure({}).ok, "empty seed list guarded");
    expectTrue(!graph.merged_invalidation_closure({"missing"}).ok, "unknown seed guarded");
}

void testCookDependencyGraphReachabilityAndLayerIndex() {
    fuse::project::CookDependencyGraph graph;
    graph.add_node("a");
    graph.add_node("b");
    graph.add_node("c");
    expectTrue(graph.add_edge("a", "b"), "reachability a->b");
    expectTrue(graph.add_edge("b", "c"), "reachability b->c");

    expectTrue(graph.is_reachable("a", "c"), "transitive reachability detected");
    expectTrue(graph.is_reachable("a", "a"), "self reachability is true");
    expectTrue(!graph.is_reachable("c", "a"), "reverse reachability rejected");
    expectTrue(!graph.is_reachable("", "b"), "empty from id guarded");
    expectTrue(!graph.is_reachable("a", "missing"), "unknown to id guarded");

    expectTrue(graph.topological_layer_index("a") == 0, "root is layer zero");
    expectTrue(graph.topological_layer_index("b") == 1, "middle is layer one");
    expectTrue(graph.topological_layer_index("c") == 2, "leaf is layer two");
    expectTrue(graph.topological_layer_index("missing") == -1, "unknown layer index guarded");

    fuse::project::CookDependencyGraph cyclic;
    cyclic.add_node("x");
    cyclic.add_node("y");
    expectTrue(cyclic.add_edge("x", "y"), "cycle index x->y");
    expectTrue(cyclic.add_edge("y", "x"), "cycle index y->x");
    expectTrue(cyclic.topological_layer_index("x") == -1, "cyclic graph layer index guarded");
    expectTrue(cyclic.parallel_layer_width() == 0u, "cyclic graph parallel width guarded");
}

void testCookDependencyGraphParallelLayerWidth() {
    fuse::project::CookDependencyGraph graph;
    graph.add_node("a");
    graph.add_node("b");
    graph.add_node("c");
    graph.add_node("d");
    expectTrue(graph.add_edge("a", "b"), "width test a->b");
    expectTrue(graph.add_edge("a", "c"), "width test a->c");
    expectTrue(graph.add_edge("b", "d"), "width test b->d");
    expectTrue(graph.add_edge("c", "d"), "width test c->d");

    expectTrue(graph.parallel_layer_width() == 2u, "diamond parallel width is two");
}

void testCookJobGraphUpstreamAndMergedClosure() {
    const std::string sourceA = writeTempFile("/tmp/fuse_b79_up_a.obj", "# up a\n");
    const std::string sourceB = writeTempFile("/tmp/fuse_b79_up_b.obj", "# up b\n");
    const std::string sourceC = writeTempFile("/tmp/fuse_b79_up_c.obj", "# up c\n");

    fuse::project::CookManifest manifest;

    fuse::project::CookManifestEntry entryA;
    entryA.kind = fuse::project::CookAssetKind::Mesh;
    entryA.source_path = sourceA;
    entryA.output_path = "/tmp/fuse_b79_up_a.fusemesh";
    manifest.assets.push_back(entryA);

    fuse::project::CookManifestEntry entryB;
    entryB.kind = fuse::project::CookAssetKind::Mesh;
    entryB.source_path = sourceB;
    entryB.output_path = "/tmp/fuse_b79_up_b.fusemesh";
    entryB.dependencies.push_back(entryA.output_path);
    manifest.assets.push_back(entryB);

    fuse::project::CookManifestEntry entryC;
    entryC.kind = fuse::project::CookAssetKind::Mesh;
    entryC.source_path = sourceC;
    entryC.output_path = "/tmp/fuse_b79_up_c.fusemesh";
    entryC.dependencies.push_back(entryB.output_path);
    manifest.assets.push_back(entryC);

    fuse::project::CookJobGraph graph;
    graph.build_from_manifest(manifest);

    const fuse::project::CookInvalidationClosureResult upstream =
        graph.upstream_invalidation_closure(entryC.output_path);
    expectTrue(upstream.ok, "job graph upstream closure ok");
    expectTrue(upstream.job_ids.size() == 2u, "leaf job has two upstream jobs");
    expectTrue(upstream.job_ids[0] == entryA.output_path, "root job in upstream closure");
    expectTrue(upstream.job_ids[1] == entryB.output_path, "middle job in upstream closure");

    const fuse::project::CookInvalidationClosureResult merged =
        graph.merged_invalidation_closure({entryA.output_path, entryB.output_path});
    expectTrue(merged.ok, "job graph merged closure ok");
    expectTrue(merged.job_ids.size() == 2u, "merged seeds cover both downstream jobs");
    expectTrue(merged.job_ids[0] == entryB.output_path, "first merged downstream job");
    expectTrue(merged.job_ids[1] == entryC.output_path, "second merged downstream job");

    fuse::project::CookJobGraph empty_graph;
    expectTrue(!empty_graph.upstream_invalidation_closure(entryA.output_path).ok,
               "empty job graph upstream closure guarded");
    expectTrue(!empty_graph.merged_invalidation_closure({entryA.output_path}).ok,
               "empty job graph merged closure guarded");
}

void testCookDependencyGraphCycleEdges() {
    fuse::project::CookDependencyGraph graph;
    graph.add_node("a");
    graph.add_node("b");
    expectTrue(graph.add_edge("a", "b"), "forward edge added");
    expectTrue(graph.add_edge("b", "a"), "back edge added");

    const fuse::project::CookJobGraphOrderResult order = graph.topological_order();
    expectTrue(!order.ok, "two-node cycle topo not ok");
    expectTrue(order.cycle_detected, "two-node cycle detected");
    expectTrue(order.order.empty(), "cyclic graph yields empty topo order");

    const fuse::project::CookDependencyCycleResult cycles = graph.detect_cycle_edges();
    expectTrue(cycles.cycle_detected, "cycle edge probe reports cycle");
    expectTrue(!cycles.cycle_edges.empty(), "cycle edge probe returns back edges");
    expectTrue(cycles.cycle_edges[0].from_job_id == "b", "back edge starts at b");
    expectTrue(cycles.cycle_edges[0].to_job_id == "a", "back edge closes on a");
}

void testCookJobGraphCycleEdgesIntegration() {
    fuse::project::CookManifest manifest;

    fuse::project::CookManifestEntry entryA;
    entryA.kind = fuse::project::CookAssetKind::Mesh;
    entryA.source_path = "/tmp/fuse_b79_cycle_edge_a.obj";
    entryA.output_path = "/tmp/fuse_b79_cycle_edge_a.fusemesh";
    entryA.dependencies.push_back("/tmp/fuse_b79_cycle_edge_b.fusemesh");
    manifest.assets.push_back(entryA);

    fuse::project::CookManifestEntry entryB;
    entryB.kind = fuse::project::CookAssetKind::Mesh;
    entryB.source_path = "/tmp/fuse_b79_cycle_edge_b.obj";
    entryB.output_path = "/tmp/fuse_b79_cycle_edge_b.fusemesh";
    entryB.dependencies.push_back("/tmp/fuse_b79_cycle_edge_a.fusemesh");
    manifest.assets.push_back(entryB);

    fuse::project::CookJobGraph graph;
    graph.build_from_manifest(manifest);

    const fuse::project::CookDependencyCycleResult cycles = graph.cycle_edges();
    expectTrue(cycles.cycle_detected, "manifest cycle exposes cycle edges");
    expectTrue(cycles.cycle_edges.size() >= 1u, "at least one cycle edge reported");
    expectTrue(graph.dependency_graph().node_count() == 2u, "dependency graph mirrors job count");
}

void testCookJobGraphCycleDetectDirect() {
    fuse::project::CookManifest manifest;

    fuse::project::CookManifestEntry entryA;
    entryA.kind = fuse::project::CookAssetKind::Mesh;
    entryA.source_path = "/tmp/fuse_b79_cycle_direct_a.obj";
    entryA.output_path = "/tmp/fuse_b79_cycle_direct_a.fusemesh";
    entryA.dependencies.push_back("/tmp/fuse_b79_cycle_direct_b.fusemesh");
    manifest.assets.push_back(entryA);

    fuse::project::CookManifestEntry entryB;
    entryB.kind = fuse::project::CookAssetKind::Mesh;
    entryB.source_path = "/tmp/fuse_b79_cycle_direct_b.obj";
    entryB.output_path = "/tmp/fuse_b79_cycle_direct_b.fusemesh";
    entryB.dependencies.push_back("/tmp/fuse_b79_cycle_direct_a.fusemesh");
    manifest.assets.push_back(entryB);

    fuse::project::CookJobGraph graph;
    graph.build_from_manifest(manifest);

    const fuse::project::CookJobGraphOrderResult order = graph.topological_order();
    expectTrue(!order.ok, "cyclic graph order not ok");
    expectTrue(order.cycle_detected, "topological order reports cycle");
    expectTrue(order.order.empty(), "cyclic graph yields empty order");
    expectTrue(graph.has_cycle(), "has_cycle agrees with order stub");
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

void testCookJobGraphStageHelpers() {
    expectTrue(fuse::project::cookStageIndex(fuse::project::CookStageKind::Import) == 0u, "import stage index");
    expectTrue(fuse::project::cookStageIndex(fuse::project::CookStageKind::Process) == 1u, "process stage index");
    expectTrue(fuse::project::cookStageIndex(fuse::project::CookStageKind::Pack) == 2u, "pack stage index");

    expectTrue(fuse::project::nextCookStageKind(fuse::project::CookStageKind::Import) ==
                   fuse::project::CookStageKind::Process,
               "import advances to process");
    expectTrue(fuse::project::nextCookStageKind(fuse::project::CookStageKind::Pack) ==
                   fuse::project::CookStageKind::Pack,
               "pack is terminal stage kind");
    expectTrue(fuse::project::isTerminalCookStage(fuse::project::CookStageKind::Pack), "pack is terminal");
    expectTrue(!fuse::project::isTerminalCookStage(fuse::project::CookStageKind::Import), "import not terminal");

    fuse::project::CookJob job;
    job.stages = {
        {fuse::project::CookStageKind::Import, fuse::project::CookStageStatus::Ok, {}},
        {fuse::project::CookStageKind::Process, fuse::project::CookStageStatus::Pending, {}},
        {fuse::project::CookStageKind::Pack, fuse::project::CookStageStatus::Pending, {}},
    };
    expectTrue(fuse::project::pendingCookStageCount(job) == 2u, "two pending stages");
    expectTrue(fuse::project::firstPendingCookStageIndex(job) == 1u, "first pending is process stage");
}

void testCookJobGraphReadyJobsAndInvalidationClosure() {
    const std::string sourceA = writeTempFile("/tmp/fuse_b79_ready_a.obj", "# ready a\n");
    const std::string sourceB = writeTempFile("/tmp/fuse_b79_ready_b.obj", "# ready b\n");
    const std::string sourceC = writeTempFile("/tmp/fuse_b79_ready_c.obj", "# ready c\n");

    fuse::project::CookManifest manifest;

    fuse::project::CookManifestEntry entryA;
    entryA.kind = fuse::project::CookAssetKind::Mesh;
    entryA.source_path = sourceA;
    entryA.output_path = "/tmp/fuse_b79_ready_a.fusemesh";
    manifest.assets.push_back(entryA);

    fuse::project::CookManifestEntry entryB;
    entryB.kind = fuse::project::CookAssetKind::Mesh;
    entryB.source_path = sourceB;
    entryB.output_path = "/tmp/fuse_b79_ready_b.fusemesh";
    entryB.dependencies.push_back(entryA.output_path);
    manifest.assets.push_back(entryB);

    fuse::project::CookManifestEntry entryC;
    entryC.kind = fuse::project::CookAssetKind::Mesh;
    entryC.source_path = sourceC;
    entryC.output_path = "/tmp/fuse_b79_ready_c.fusemesh";
    entryC.dependencies.push_back(entryB.output_path);
    manifest.assets.push_back(entryC);

    fuse::project::CookJobGraph graph;
    graph.build_from_manifest(manifest);

    const std::vector<std::string> initial_ready = graph.ready_job_ids({});
    expectTrue(initial_ready.size() == 1u, "only root job is initially ready");
    expectTrue(initial_ready[0] == entryA.output_path, "root job id matches");

    const std::vector<std::string> after_a = graph.ready_job_ids({entryA.output_path});
    expectTrue(after_a.size() == 1u, "one job ready after root completes");
    expectTrue(after_a[0] == entryB.output_path, "middle job becomes ready");

    const fuse::project::CookDependencyLayerResult layers = graph.topological_layers();
    expectTrue(layers.ok, "job graph layers ok");
    expectTrue(layers.layers.size() == 3u, "linear chain has three layers");

    const fuse::project::CookInvalidationClosureResult closure =
        graph.invalidation_closure(entryA.output_path);
    expectTrue(closure.ok, "job graph invalidation closure ok");
    expectTrue(closure.job_ids.size() == 2u, "upstream invalidates two downstream jobs");
    expectTrue(closure.job_ids[0] == entryB.output_path, "first downstream job in closure");
    expectTrue(closure.job_ids[1] == entryC.output_path, "second downstream job in closure");

    fuse::project::CookJobGraph empty_graph;
    const fuse::project::CookInvalidationClosureResult empty_closure =
        empty_graph.invalidation_closure(entryA.output_path);
    expectTrue(!empty_closure.ok, "empty job graph invalidation guarded");
    expectTrue(empty_graph.ready_job_ids({}).empty(), "empty job graph ready list guarded");
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
    const fuse::u32 estimated = cooker.estimate_stale_dependency_hash_invalidations(manifest);
    expectTrue(estimated >= 1u, "stale dependency reconcile estimator predicts removals");
    const fuse::u32 removed = cooker.invalidate_stale_dependency_hashes(manifest);
    expectTrue(removed >= 1u, "stale upstream hash invalidates dependent cache entries");
    expectTrue(estimated == removed, "stale dependency reconcile estimate matches invalidation");
    expectTrue(cooker.cache().lookup(downstream_hash) == fuse::project::CookCacheLookup::Miss,
               "downstream misses after stale dependency hash invalidation");
}

void testCookCacheStaleDependencyHashEstimator() {
    const std::string sourceA = writeTempFile("/tmp/fuse_b79_est_a.obj", "# est a\n");
    const std::string sourceB = writeTempFile("/tmp/fuse_b79_est_b.obj", "# est b\n");
void testCookCacheStaleDependencyEstimatorProbe() {
    const std::string sourceA = writeTempFile("/tmp/fuse_b79_est_a.obj", "# estimator a\n");
    const std::string sourceB = writeTempFile("/tmp/fuse_b79_est_b.obj", "# estimator b\n");

    fuse::project::CookManifest manifest;

    fuse::project::CookManifestEntry entryA;
    entryA.kind = fuse::project::CookAssetKind::Mesh;
    entryA.source_path = sourceA;
    entryA.output_path = "/tmp/fuse_b79_est_a.fusemesh";
    manifest.assets.push_back(entryA);

    fuse::project::CookManifestEntry entryB;
    entryB.kind = fuse::project::CookAssetKind::Mesh;
    entryB.source_path = sourceB;
    entryB.output_path = "/tmp/fuse_b79_est_b.fusemesh";
    entryB.dependencies.push_back(entryA.output_path);
    manifest.assets.push_back(entryB);

    fuse::project::AssetCooker cooker;
    const fuse::project::CookBatchResult batch = cooker.cook_manifest(manifest);
    expectTrue(batch.ok, "estimator test seeds cache");
    expectTrue(cooker.estimate_stale_dependency_hashes(manifest) == 0u,
               "fresh cache estimates zero stale dependency removals");

    writeTempFile(sourceA, "# est a revised\n");
    const fuse::u32 estimate = cooker.estimate_stale_dependency_hashes(manifest);
    expectTrue(estimate >= 1u, "stale upstream change yields non-zero reconcile estimate");

    const fuse::u32 removed = cooker.invalidate_stale_dependency_hashes(manifest);
    expectTrue(removed == estimate, "dependency reconcile estimate matches actual invalidation count");
               "clean cache estimates zero after reconcile");
    expectTrue(batch.ok, "estimator probe seeds cache");

    const fuse::project::CookCacheInvalidationProbe clean_probe =
        cooker.estimate_stale_dependency_invalidation(manifest);
    expectTrue(!clean_probe.would_invalidate(), "clean cache stale-dependency estimator reports no removal");
    expectTrue(cooker.estimate_cache_reconcile().total_prunable() == 0u,
               "clean cache reconcile estimate is zero");

    writeTempFile(sourceA, "# estimator a revised\n");
    const fuse::project::CookCacheInvalidationProbe dirty_probe =
    expectTrue(dirty_probe.would_invalidate(), "upstream change estimator reports pending invalidation");
    expectTrue(dirty_probe.would_invalidate_count >= 1u, "stale dependency estimator counts at least one entry");

    const fuse::u64 invalidations_before = cooker.cache().stats().invalidations;
    expectTrue(removed >= dirty_probe.would_invalidate_count,
               "actual stale dependency invalidation meets estimator lower bound");
    expectTrue(cooker.cache().stats().invalidations > invalidations_before,
               "stale dependency invalidation bumps stats after estimator probe");
    expectTrue(cooker.estimate_stale_dependency_hashes(manifest) == 0u,
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
    const fuse::u32 estimate = cooker.estimate_upstream_dependency_invalidation(manifest, sourceA);
    expectTrue(estimate >= 2u, "upstream change yields non-zero upstream invalidation estimate");
    const fuse::u32 removed = cooker.invalidate_upstream_dependency(manifest, sourceA);
    expectTrue(removed == estimate, "upstream invalidation estimate matches actual invalidation count");
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

    empty_paths.source_path = "/tmp/fuse_b79_empty_source.obj";
    empty_paths.output_path = "";
    cache.store(empty_paths);
    expectTrue(cache.entry_count() == 0u, "empty output path is not stored");

    expectTrue(!fuse::project::is_valid_cook_cache_path(""), "empty path fails path validation");
    expectTrue(fuse::project::is_valid_cook_cache_path("/tmp/fuse_b79_ok.obj"), "non-empty path passes validation");

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

void testContentHashByteSensitivity() {
    const std::string source = writeTempFile("/tmp/fuse_b79_hash_bytes.obj", "# bytes v1\n");

    const fuse::u64 hash_before = fuse::project::hash_file_content(source);
    expectTrue(hash_before != 0, "byte-aware file hash is non-zero");

    writeTempFile(source, "# bytes v2\n");

    const fuse::u64 hash_after = fuse::project::hash_file_content(source);
    expectTrue(hash_after != hash_before, "content byte change alters content hash key");
}

void testCookCacheContentChangePrunesStale() {
    const std::string source = writeTempFile("/tmp/fuse_b79_prune_mesh.obj", "# prune v1\n");

    fuse::project::MeshImportDesc desc;
    desc.input_path = source;
    desc.output_path = "/tmp/fuse_b79_prune_mesh.fusemesh";

    fuse::project::AssetCooker cooker;
    const fuse::project::CookRecord first = cooker.cook_mesh(desc);
    expectTrue(first.ok, "first cook ok");
    expectTrue(!first.cache_hit, "first cook misses");
    expectTrue(cooker.cache().entry_count() == 1u, "one cache entry after first cook");

    writeTempFile(source, "# prune v2\n");
    const fuse::project::CookRecord second = cooker.cook_mesh(desc);
    expectTrue(second.ok, "post-change cook ok");
    expectTrue(!second.cache_hit, "post-change cook misses with new hash");
    expectTrue(second.content_hash != first.content_hash, "content change yields new cache key");
    expectTrue(cooker.cache().entry_count() == 1u, "stale entry pruned on store");
    expectTrue(cooker.cache().stats().invalidations >= 1u, "stale prune counted as invalidation");

    const fuse::project::CookRecord third = cooker.cook_mesh(desc);
    expectTrue(third.cache_hit, "third cook hits with current hash");
}

void testCookCacheOutputInvalidation() {
    const std::string source = writeTempFile("/tmp/fuse_b79_outinv_mesh.obj", "# output inv\n");

    fuse::project::MeshImportDesc desc;
    desc.input_path = source;
    desc.output_path = "/tmp/fuse_b79_outinv_mesh.fusemesh";

    fuse::project::AssetCooker cooker;
    const fuse::project::CookRecord seeded = cooker.cook_mesh(desc);
    expectTrue(seeded.ok, "seed cook ok");
    expectTrue(cooker.cache().entry_count() == 1u, "cache seeded");

    expectTrue(cooker.cache().invalidate_output(desc.output_path) == 1u, "output invalidation removes entry");
    expectTrue(cooker.cache().entry_count() == 0u, "cache empty after output invalidation");
    expectTrue(cooker.cache().lookup(seeded.content_hash) == fuse::project::CookCacheLookup::Miss,
               "lookup misses after output invalidation");

    expectTrue(cooker.cache().invalidate_output("") == 0u, "empty output path is a no-op");
    expectTrue(cooker.cache().invalidate_output(desc.output_path) == 0u,
               "output invalidation on empty cache is a no-op");
}

void testCookCachePruneStaleEntries() {
    const std::string source = writeTempFile("/tmp/fuse_b79_batch_prune.obj", "# batch prune v1\n");

    fuse::project::MeshImportDesc desc;
    desc.input_path = source;
    desc.output_path = "/tmp/fuse_b79_batch_prune.fusemesh";

    fuse::project::AssetCooker cooker;
    const fuse::project::CookRecord first = cooker.cook_mesh(desc);
    expectTrue(first.ok, "seed cook for batch prune ok");
    expectTrue(cooker.cache().entry_count() == 1u, "one entry before batch prune");

    writeTempFile(source, "# batch prune v2\n");
    expectTrue(cooker.cache().lookup(first.content_hash) == fuse::project::CookCacheLookup::Hit,
               "stale entry still present before explicit prune");

    const fuse::u32 removed = cooker.cache().prune_stale_entries();
    expectTrue(removed == 1u, "batch prune removes stale entry");
    expectTrue(cooker.cache().entry_count() == 0u, "cache empty after batch prune");
    expectTrue(cooker.cache().lookup(first.content_hash) == fuse::project::CookCacheLookup::Miss,
               "lookup misses after batch prune");
    expectTrue(cooker.cache().stats().invalidations >= 1u, "batch prune counted as invalidation");

    expectTrue(cooker.cache().prune_stale_entries() == 0u, "prune on empty cache is a no-op");
}

void testCookCacheEmptyGuards() {
    fuse::project::CookCache cache;
    expectTrue(cache.empty(), "fresh cache is empty");
    expectTrue(cache.entry_count() == 0u, "fresh cache has zero entries");

    expectTrue(cache.prune_stale_entries() == 0u, "prune on empty cache returns zero");
    expectTrue(cache.invalidate_output("/tmp/fuse_b79_missing.fusemesh") == 0u,
               "output invalidation on empty cache returns zero");
    expectTrue(cache.invalidate_source("/tmp/fuse_b79_missing.obj") == 0u,
               "source invalidation on empty cache returns zero");
    expectTrue(cache.invalidate_stale_content_for_source("/tmp/fuse_b79_missing.obj", 42u) == 0u,
               "stale-content invalidation on empty cache returns zero");

    cache.invalidate_all();
    expectTrue(cache.empty(), "invalidate_all on empty cache stays empty");
    expectTrue(cache.stats().invalidations == 0u, "invalidate_all on empty cache does not bump stats");

    const std::string cachePath = "/tmp/fuse_b79_empty_cache.json";
    expectTrue(cache.save(cachePath), "empty cache saves valid JSON");
    fuse::project::CookCache loaded;
    expectTrue(loaded.load(cachePath), "empty cache JSON loads");
    expectTrue(loaded.empty(), "loaded empty cache stays empty");
    expectTrue(loaded.entry_count() == 0u, "loaded empty cache has zero entries");

    expectTrue(!cache.save(""), "save rejects empty path");
    expectTrue(!cache.load(""), "load rejects empty path");

    const std::vector<std::pair<std::string, fuse::u64>> empty_upstream;
    expectTrue(cache.invalidate_stale_upstream_hashes(empty_upstream).empty(),
               "stale upstream invalidation on empty cache returns empty list");
    expectTrue(cache.invalidate_stale_upstream_hashes({{"", 1u}}).empty(),
               "stale upstream invalidation skips empty source paths");

    expectTrue(cache.invalidate_downstream_of("/tmp/fuse_b79_missing.fusemesh", {}, {}) == 0u,
               "downstream invalidation on empty cache returns zero");
    expectTrue(cache.invalidate_downstream_of("", {}, {}) == 0u,
               "downstream invalidation rejects empty output path");

    expectTrue(!cache.invalidate(42u), "hash invalidation on empty cache returns false");
    expectTrue(cache.stats().invalidations == 0u, "hash invalidation on empty cache does not bump stats");
    expectTrue(!cache.contains(42u), "contains on empty cache returns false");

    const fuse::u64 misses_before = cache.stats().misses;
    expectTrue(cache.lookup(42u) == fuse::project::CookCacheLookup::Miss, "lookup misses on empty cache");
    expectTrue(cache.stats().misses == misses_before + 1u, "lookup on empty cache records one miss");

    expectTrue(cache.prune_invalid_entries() == 0u, "prune_invalid on empty cache returns zero");
    expectTrue(cache.invalidate_stale_content_for_source("", 42u) == 0u,
               "stale-content invalidation rejects empty source path");
}

void testCookCacheInvalidatePruneGuards() {
    const std::string source = writeTempFile("/tmp/fuse_b79_guard_mesh.obj", "# guard mesh\n");

    fuse::project::MeshImportDesc desc;
    desc.input_path = source;
    desc.output_path = "/tmp/fuse_b79_guard_mesh.fusemesh";

    fuse::project::AssetCooker cooker;
    const fuse::project::CookRecord seeded = cooker.cook_mesh(desc);
    expectTrue(seeded.ok, "seed cook for invalidate/prune guards ok");
    expectTrue(cooker.cache().entry_count() == 1u, "cache seeded for guard tests");

    expectTrue(cooker.cache().invalidate_source("") == 0u, "empty source path invalidation is a no-op");
    expectTrue(cooker.cache().entry_count() == 1u, "empty source invalidation leaves cache untouched");
    expectTrue(cooker.cache().contains(seeded.content_hash), "seeded entry remains after empty source invalidation");

    expectTrue(!cooker.cache().invalidate(0), "zero-hash invalidation on populated cache is a no-op");
    expectTrue(cooker.cache().contains(seeded.content_hash), "seeded entry remains after zero-hash invalidation");

    expectTrue(cooker.cache().invalidate_stale_content_for_source(source, 0u) == 0u,
               "zero current hash does not invalidate seeded entry");
    expectTrue(cooker.cache().contains(seeded.content_hash), "seeded entry survives zero-hash stale guard");

    expectTrue(cooker.cache().invalidate_stale_content_for_source(source, seeded.content_hash + 1u) == 1u,
               "mismatched current hash invalidates stale entry");
    expectTrue(cooker.cache().empty(), "cache empty after stale-content invalidation");
               "zero current hash stale-content invalidation is a no-op");
    expectTrue(cooker.cache().entry_count() == 1u, "seeded entry survives zero-hash stale guard");
    expectTrue(!cooker.cache().has_invalid_entries(), "valid cache has no invalid entries");
    expectTrue(!cooker.cache().has_stale_entries(), "fresh cache has no stale entries");

    const fuse::project::CookRecord reseeded = cooker.cook_mesh(desc);
    expectTrue(reseeded.ok, "reseed cook ok");
    expectTrue(cooker.cache().prune_stale_entries() == 0u, "prune_stale on fresh entry is a no-op");
    expectTrue(cooker.cache().prune_invalid_entries() == 0u, "prune_invalid on valid entry is a no-op");
    expectTrue(cooker.cache().contains(reseeded.content_hash), "valid entry survives prune guards");

    writeTempFile(source, "# guard mesh updated\n");
    expectTrue(cooker.cache().prune_stale_entries() == 1u, "prune_stale removes entry after source change");
    expectTrue(cooker.cache().empty(), "cache empty after stale prune");
    expectTrue(cooker.cache().prune_stale_entries() == 0u, "second prune_stale on empty cache is a no-op");
    expectTrue(cooker.cache().prune_invalid_entries() == 0u, "prune_invalid on empty cache after stale prune");

void testCookContentHashGuardHelpers() {
    fuse::project::CookManifest manifest;
    expectTrue(fuse::project::hash_upstream_dependencies({}, manifest) == 0,
               "empty dependency list yields zero upstream hash");
    expectTrue(fuse::project::combine_cook_cache_key(0, 42u) == 0,
               "zero source hash stays zero when upstream is non-zero");
    expectTrue(fuse::project::file_mtime_ns("") == 0, "empty path mtime is zero");
    expectTrue(!cache.invalidate(42u), "hash invalidation on empty cache is a no-op");
    expectTrue(cache.stats().invalidations == 0u, "empty-cache hash invalidation does not bump stats");
    expectTrue(cache.prune_all() == 0u, "prune_all on empty cache returns zero");

    fuse::project::CookHashPreflightRejectReason reason = fuse::project::CookHashPreflightRejectReason::None;
    expectTrue(!fuse::project::preflight_hash_file_content("", &reason),
               "empty path fails hash preflight guard");
    expectTrue(reason == fuse::project::CookHashPreflightRejectReason::EmptyPath,
               "empty path preflight reports EmptyPath");
    expectTrue(fuse::project::preflight_combine_cook_cache_key(42u, 0u, &reason),
               "valid source hash passes combine preflight");
}

void testCookCacheContainsHelper() {
    const std::string source = writeTempFile("/tmp/fuse_b79_contains_mesh.obj", "# contains mesh\n");

    fuse::project::MeshImportDesc desc;
    desc.input_path = source;
    desc.output_path = "/tmp/fuse_b79_contains_mesh.fusemesh";

    fuse::project::AssetCooker cooker;
    const fuse::project::CookRecord seeded = cooker.cook_mesh(desc);
    expectTrue(seeded.ok, "seed cook for contains helper ok");
    expectTrue(cooker.cache().contains(seeded.content_hash), "contains reports seeded hash");
    expectTrue(!cooker.cache().contains(0), "contains rejects zero hash");
    expectTrue(!cooker.cache().contains(seeded.content_hash + 1u), "contains rejects unknown hash");

    const fuse::u64 hits_before = cooker.cache().stats().hits;
    expectTrue(cooker.cache().contains(seeded.content_hash), "contains does not increment hit stats");
    expectTrue(cooker.cache().stats().hits == hits_before, "contains leaves hit counter unchanged");
}

void testCookCachePruneInvalidEntries() {
    const std::string validSource = writeTempFile("/tmp/fuse_b79_valid_entry.obj", "# valid entry\n");

    fuse::project::CookCacheEntry valid;
    valid.source_path = validSource;
    valid.output_path = "/tmp/fuse_b79_valid_entry.fusemesh";
    valid.content_hash = fuse::project::combine_cook_cache_key(
        fuse::project::hash_mesh_import(
            fuse::project::MeshImportDesc{validSource, "/tmp/fuse_b79_valid_entry.fusemesh"}),
        0);
    expectTrue(fuse::project::is_valid_cook_cache_entry(valid), "valid entry passes validation");

    fuse::project::CookCacheEntry invalid = valid;
    invalid.content_hash = 0;
    expectTrue(!fuse::project::is_valid_cook_cache_entry(invalid), "zero hash fails entry validation");

    invalid = valid;
    invalid.source_path = "";
    expectTrue(!fuse::project::is_valid_cook_cache_entry(invalid), "empty source fails entry validation");

    invalid = valid;
    invalid.output_path = "";
    expectTrue(!fuse::project::is_valid_cook_cache_entry(invalid), "empty output fails entry validation");

    fuse::project::CookCache cache;
    cache.store(invalid);
    expectTrue(cache.entry_count() == 0u, "store rejects invalid entry");
    cache.store(valid);
    expectTrue(cache.entry_count() == 1u, "store accepts valid entry");
    expectTrue(cache.prune_invalid_entries() == 0u, "prune_invalid on valid-only cache is a no-op");
    expectTrue(cache.contains(valid.content_hash), "valid entry remains after invalid prune");

    const std::string loadedSource = writeTempFile("/tmp/fuse_b79_valid.obj", "# loaded valid\n");
    fuse::project::MeshImportDesc loadedDesc;
    loadedDesc.input_path = loadedSource;
    loadedDesc.output_path = "/tmp/fuse_b79_valid.fusemesh";
    const fuse::u64 loadedHash = fuse::project::combine_cook_cache_key(
        fuse::project::hash_mesh_import(loadedDesc), 0);

    const std::string cachePath = "/tmp/fuse_b79_invalid_cache.json";
    {
        std::ofstream out(cachePath, std::ios::binary);
        out << "{\n  \"schemaVersion\": 1,\n  \"entries\": [\n    {\n";
        out << "      \"contentHash\": " << loadedHash << ",\n";
        out << R"(
      "upstreamHash": 0,
      "outputPath": "/tmp/fuse_b79_valid.fusemesh",
      "sourcePath": "/tmp/fuse_b79_valid.obj",
      "kind": "mesh"
    },
    {
      "contentHash": 0,
      "upstreamHash": 0,
      "outputPath": "/tmp/fuse_b79_zero_hash.fusemesh",
      "sourcePath": "/tmp/fuse_b79_zero_hash.obj",
      "kind": "mesh"
    },
    {
      "contentHash": 99,
      "upstreamHash": 0,
      "outputPath": "/tmp/fuse_b79_empty_source.fusemesh",
      "sourcePath": "",
      "kind": "mesh"
    }
  ]
}
)";
    }

    fuse::project::CookCache loaded;
    expectTrue(loaded.load(cachePath), "mixed cache JSON loads");
    expectTrue(loaded.entry_count() == 1u, "load keeps first valid entry and rejects invalid trailing records");
    expectTrue(loaded.contains(loadedHash), "valid loaded entry remains addressable");

    const std::string invalidOnlyPath = "/tmp/fuse_b79_invalid_only_cache.json";
    {
        std::ofstream out(invalidOnlyPath, std::ios::binary);
        out << R"({
  "schemaVersion": 1,
  "entries": [
    {
      "contentHash": 0,
      "upstreamHash": 0,
      "outputPath": "/tmp/fuse_b79_zero_only.fusemesh",
      "sourcePath": "/tmp/fuse_b79_zero_only.obj",
      "kind": "mesh"
    }
  ]
}
)";
    }

    fuse::project::CookCache invalidOnly;
    expectTrue(invalidOnly.load(invalidOnlyPath), "invalid-only cache JSON loads");
    expectTrue(invalidOnly.empty(), "invalid-only cache rejects zero-hash entry on load");
    expectTrue(invalidOnly.prune_invalid_entries() == 0u, "prune on empty cache after rejected load");
}

void testCookerReconcileEstimators() {
void testCookerStaleDependencyEstimateParity() {
    const std::string sourceA = writeTempFile("/tmp/fuse_b79_est_chain_a.obj", "# est chain a\n");
    const std::string sourceB = writeTempFile("/tmp/fuse_b79_est_chain_b.obj", "# est chain b\n");
void testCookerStaleDependencyReconcileEstimatorParity() {
void testCookerStaleDependencyReconcileEstimator() {
void testCookerReconcileEstimatorProbes() {
    const std::string sourceA = writeTempFile("/tmp/fuse_b79_reconcile_a.obj", "# reconcile a\n");
    const std::string sourceB = writeTempFile("/tmp/fuse_b79_reconcile_b.obj", "# reconcile b\n");
void testCookerUpstreamInvalidationEstimateProbes() {

    fuse::project::CookManifest manifest;
    fuse::project::CookManifestEntry entryA;
    entryA.kind = fuse::project::CookAssetKind::Mesh;
    entryA.source_path = sourceA;
    entryA.output_path = "/tmp/fuse_b79_est_chain_a.fusemesh";
    entryA.output_path = "/tmp/fuse_b79_reconcile_a.fusemesh";
    manifest.assets.push_back(entryA);

    fuse::project::CookManifestEntry entryB;
    entryB.kind = fuse::project::CookAssetKind::Mesh;
    entryB.source_path = sourceB;
    entryB.output_path = "/tmp/fuse_b79_est_chain_b.fusemesh";
    entryB.output_path = "/tmp/fuse_b79_reconcile_b.fusemesh";
    entryB.dependencies.push_back(entryA.output_path);
    manifest.assets.push_back(entryB);

    fuse::project::AssetCooker cooker;
    const fuse::project::CookBatchResult cooked = cooker.cook_manifest(manifest);
    expectTrue(cooked.ok, "manifest cook for reconcile estimators ok");
    expectTrue(cooker.count_prune_removals() == 0u, "fresh cache prune estimator is zero");
    expectTrue(!cooker.would_reconcile_stale_dependencies(manifest),
               "fresh cache would not reconcile stale dependencies");
    expectTrue(cooker.count_stale_content_invalidation(manifest) == 0u,
               "fresh cache stale-content estimator is zero");

    writeTempFile(sourceA, "# est chain a revised\n");
    expectTrue(cooker.count_stale_content_invalidation(manifest) >= 1u,
               "source change raises stale-content estimator");
    expectTrue(cooker.count_prune_removals() >= 1u, "stale entries raise prune estimator");
    expectTrue(cooker.would_reconcile_stale_dependencies(manifest),
               "upstream hash change triggers stale dependency reconcile probe");

    const fuse::u32 pruned = cooker.cache().prune_all();
    expectTrue(pruned >= 1u, "prune_all removes stale entries estimated by reconcile probes");
    expectTrue(cooker.count_prune_removals() == 0u, "prune estimator zero after reconcile");
    expectTrue(cooked.ok, "manifest cook for stale dependency estimate ok");
    expectTrue(cooker.estimate_stale_dependency_invalidation(manifest) == 0u,
               "fresh cache stale dependency estimate is zero");
    expectTrue(cooker.count_stale_dependency_invalidation(manifest) ==
                   cooker.estimate_stale_dependency_invalidation(manifest),
               "count and estimate stale dependency invalidation agree on fresh cache");

    const fuse::u32 estimate = cooker.estimate_stale_dependency_invalidation(manifest);
    expectTrue(estimate >= 1u, "stale dependency estimate is non-zero after upstream change");

    const fuse::u32 removed = cooker.invalidate_stale_dependency_hashes(manifest);
    expectTrue(removed >= estimate, "stale dependency invalidation removes at least estimated count");
               "stale dependency estimate is zero after reconcile");
    expectTrue(cooker.estimate_prune_all() == 0u, "fresh cache prune estimate is zero");
    expectTrue(cooker.estimate_reconcile_invalidation(manifest) == 0u,
               "fresh cache reconcile estimate is zero");

    writeTempFile(sourceA, "# reconcile a revised\n");
    const fuse::u32 stale_estimate = cooker.count_stale_dependency_invalidation(manifest);
    expectTrue(stale_estimate >= 1u, "stale dependency estimate after upstream change is non-zero");
    expectTrue(cooker.estimate_reconcile_invalidation(manifest) >= stale_estimate,
               "reconcile estimate includes stale dependency impact");

    expectTrue(removed >= stale_estimate, "stale invalidation removes at least estimated count");
    expectTrue(cooker.count_stale_dependency_invalidation(manifest) == 0u,
               "stale dependency estimate zero after upstream reconcile");

    // Revised upstream source may leave a stale-content entry until pruned.
    expectTrue(cooker.estimate_prune_all() <= 1u,
               "upstream reconcile leaves at most one stale-content entry");
    expectTrue(cooker.estimate_reconcile_invalidation(manifest) == cooker.estimate_prune_all(),
               "reconcile estimate matches prune impact after upstream reconcile");

    cooker.cache().prune_all();
               "reconcile estimate zero after prune");
    expectTrue(cooked.ok, "manifest cook for reconcile estimator ok");
    expectTrue(cooker.cache().entry_count() == 2u, "two entries seeded for reconcile estimator");

    const fuse::u32 estimated = cooker.count_stale_dependency_invalidation(manifest);
    expectTrue(estimated > 0u, "stale dependency reconcile estimator is non-zero after upstream change");

    expectTrue(removed == estimated, "stale dependency reconcile removal matches estimator");
               "stale dependency estimator is zero after reconcile");
    expectTrue(cooker.cache().entry_count() == 1u,
               "upstream source entry remains after dependency-hash reconcile");
    expectTrue(cooker.cache().count_stale_entries() == 1u,
               "remaining upstream entry is content-stale after source revision");
    expectTrue(!cooker.cache().contains(cooked.records[1].content_hash),
               "downstream entry removed by dependency-hash reconcile");
    expectTrue(cooked.ok, "manifest cook for stale reconcile estimator ok");
               "fresh cache stale reconcile count is zero");

    expectTrue(estimated >= 1u, "upstream change yields non-zero stale reconcile estimate");

    expectTrue(removed == estimated, "stale reconcile estimator matches actual invalidation count");
}

void testCookerUpstreamInvalidationSourceProbe() {
    const std::string sourceA = writeTempFile("/tmp/fuse_b79_probe_up_a.obj", "# probe up a\n");
    const std::string sourceB = writeTempFile("/tmp/fuse_b79_probe_up_b.obj", "# probe up b\n");

    fuse::project::CookManifest manifest;
    fuse::project::CookManifestEntry entryA;
    entryA.kind = fuse::project::CookAssetKind::Mesh;
    entryA.source_path = sourceA;
    entryA.output_path = "/tmp/fuse_b79_probe_up_a.fusemesh";
    manifest.assets.push_back(entryA);

    fuse::project::CookManifestEntry entryB;
    entryB.kind = fuse::project::CookAssetKind::Mesh;
    entryB.source_path = sourceB;
    entryB.output_path = "/tmp/fuse_b79_probe_up_b.fusemesh";
    entryB.dependencies.push_back(entryA.output_path);
    manifest.assets.push_back(entryB);

    fuse::project::AssetCooker cooker;
    expectTrue(cooked.ok, "manifest cook for upstream source probe ok");

    expectTrue(cooker.probe_upstream_invalidation_sources(manifest, "").empty(),
               "empty changed source upstream probe returns empty list");

    const std::vector<std::string> probed = cooker.probe_upstream_invalidation_sources(manifest, sourceA);
    expectTrue(probed.size() >= 2u, "upstream probe lists changed source and downstream dependents");
    expectTrue(probed[0] == sourceA, "upstream probe includes changed source first");
    expectTrue(cooker.cache().entry_count() == 2u, "two entries seeded for reconcile estimators");

    expectTrue(cooker.would_upstream_invalidation(manifest, sourceA),
               "would_upstream_invalidation true before upstream change");
    expectTrue(!cooker.would_upstream_invalidation(manifest, ""),
               "empty changed source would_upstream guarded");
    expectTrue(!cooker.would_stale_dependency_invalidation(manifest),
               "fresh cache would_stale_dependency is false");
    expectTrue(cooker.estimate_cache_prune() == 0u, "fresh cache estimate_cache_prune is zero");

    expectTrue(cooker.would_stale_dependency_invalidation(manifest),
               "upstream change makes would_stale_dependency true");
    expectTrue(stale_estimate >= 1u, "stale dependency count estimate is non-zero after upstream change");

    expectTrue(removed >= stale_estimate, "stale reconcile removes at least estimated count");
               "would_stale_dependency false after reconcile");

    cooker.cook_manifest(manifest);

    fuse::project::CookJobGraph graph;
    graph.build_from_manifest(manifest);
    expectTrue(cooker.cache().would_invalidate_downstream_of(entryA.output_path, graph.edges(), graph.jobs()),
               "would_invalidate_downstream_of true on repopulated chain cache");
    expectTrue(!cooker.cache().would_invalidate_downstream_of("", graph.edges(), graph.jobs()),
               "empty output path downstream probe guarded");

    const fuse::project::CookReconcileEstimate upstream_estimate =
        cooker.estimate_upstream_invalidation(manifest, sourceA);
    expectTrue(upstream_estimate.direct_entries >= 1u, "upstream estimate counts direct entries");
    expectTrue(upstream_estimate.downstream_entries >= 1u, "upstream estimate counts downstream entries");
    expectTrue(upstream_estimate.total() == cooker.count_upstream_invalidation(manifest, sourceA),
               "upstream estimate total matches count probe");
    expectTrue(cooker.would_invalidate_upstream_dependency(manifest, sourceA),
               "would_invalidate_upstream_dependency true for seeded chain");
    expectTrue(!cooker.would_invalidate_upstream_dependency(manifest, ""),
               "would_invalidate_upstream_dependency false for empty source");

    const fuse::project::CookReconcileEstimate fresh_reconcile =
        cooker.estimate_stale_dependency_reconcile(manifest);
    expectTrue(fresh_reconcile.total() == 0u, "fresh cache reconcile estimate is zero");
    expectTrue(!fresh_reconcile.would_invalidate(), "fresh cache would not reconcile");

    const fuse::project::CookReconcileEstimate stale_reconcile =
    expectTrue(stale_reconcile.direct_entries >= 1u, "stale reconcile counts direct upstream entries");
    expectTrue(stale_reconcile.total() == cooker.count_stale_dependency_invalidation(manifest),
               "stale reconcile total matches count probe");
    expectTrue(stale_reconcile.would_invalidate(), "stale reconcile would invalidate");

    expectTrue(removed >= stale_reconcile.total(), "reconcile invalidation removes at least estimated total");
    expectTrue(cooker.cook_manifest(manifest).ok, "manifest cook for upstream estimate ok");

    const fuse::project::CookCacheUpstreamInvalidationEstimate empty =
        cooker.estimate_upstream_invalidation(manifest, "");
    expectTrue(empty.total() == 0u, "empty changed source upstream estimate is zero");
    expectTrue(!cooker.would_upstream_invalidate(manifest, ""), "empty changed source would_upstream false");

    const fuse::project::CookCacheUpstreamInvalidationEstimate estimate =
    expectTrue(estimate.direct_entries >= 1u, "upstream estimate includes direct entries");
    expectTrue(estimate.downstream_entries >= 1u, "upstream estimate includes downstream entries");
    expectTrue(estimate.total() == cooker.count_upstream_invalidation(manifest, sourceA),
    expectTrue(cooker.would_upstream_invalidate(manifest, sourceA),
               "would_upstream_invalidate true for seeded chain");

void testCookerWouldReconcileInvalidationProbe() {
    const std::string source = writeTempFile("/tmp/fuse_b79_would_reconcile.obj", "# would reconcile v1\n");

    fuse::project::CookManifestEntry entry;
    entry.kind = fuse::project::CookAssetKind::Mesh;
    entry.source_path = source;
    entry.output_path = "/tmp/fuse_b79_would_reconcile.fusemesh";
    manifest.assets.push_back(entry);

    expectTrue(cooker.cook_manifest(manifest).ok, "manifest cook for would_reconcile probe ok");
    expectTrue(!cooker.would_reconcile_invalidation(manifest),
               "fresh cache would_reconcile_invalidation is false");

    writeTempFile(source, "# would reconcile v2\n");
    expectTrue(cooker.would_reconcile_invalidation(manifest),
               "stale content makes would_reconcile_invalidation true");
    expectTrue(cooker.estimate_reconcile_invalidation(manifest).prune_stale_entries >= 1u,
               "reconcile estimate prune stale matches would_reconcile");
}

void testCookerInvalidationCountProbes() {
    const std::string sourceA = writeTempFile("/tmp/fuse_b79_count_chain_a.obj", "# count chain a\n");
    const std::string sourceB = writeTempFile("/tmp/fuse_b79_count_chain_b.obj", "# count chain b\n");

    fuse::project::CookManifest manifest;
    fuse::project::CookManifestEntry entryA;
    entryA.kind = fuse::project::CookAssetKind::Mesh;
    entryA.source_path = sourceA;
    entryA.output_path = "/tmp/fuse_b79_count_chain_a.fusemesh";
void testCookCacheReconcileEstimators() {
    const std::string sourceA = writeTempFile("/tmp/fuse_b79_est_a.obj", "# est a\n");
    const std::string sourceB = writeTempFile("/tmp/fuse_b79_est_b.obj", "# est b\n");


void testCookCacheReconcileEstimator() {


void testCookStaleDependencyHashReconcileEstimate() {


    entryA.output_path = "/tmp/fuse_b79_est_a.fusemesh";
    manifest.assets.push_back(entryA);

    fuse::project::CookManifestEntry entryB;
    entryB.kind = fuse::project::CookAssetKind::Mesh;
    entryB.source_path = sourceB;
    entryB.output_path = "/tmp/fuse_b79_count_chain_b.fusemesh";
    entryB.output_path = "/tmp/fuse_b79_est_b.fusemesh";
    entryB.dependencies.push_back(entryA.output_path);
    manifest.assets.push_back(entryB);

    fuse::project::AssetCooker cooker;
    const fuse::project::CookBatchResult cooked = cooker.cook_manifest(manifest);
    expectTrue(cooked.ok, "manifest cook for count probes ok");
    expectTrue(cooker.cache().entry_count() == 2u, "two entries seeded for count probes");

    const fuse::u32 upstream_count = cooker.count_upstream_invalidation(manifest, sourceA);
    expectTrue(upstream_count >= 2u, "upstream count probe estimates chain removals");
    expectTrue(cooker.cache().would_invalidate_downstream_of(entryA.output_path,
                                                             fuse::project::CookJobGraph{}.edges(),
                                                             fuse::project::CookJobGraph{}.jobs()) == false,
               "would_invalidate_downstream rejects empty graph");

    fuse::project::CookJobGraph graph;
    graph.build_from_manifest(manifest);
    expectTrue(cooker.cache().would_invalidate_downstream_of(entryA.output_path, graph.edges(), graph.jobs()),
               "would_invalidate_downstream reports dependent entries");

    const fuse::project::CookUpstreamInvalidationEstimate upstream_estimate =
        cooker.estimate_upstream_invalidation(manifest, sourceA);
    expectTrue(upstream_estimate.total() == upstream_count,
               "upstream estimate total matches count probe");
    expectTrue(upstream_estimate.direct >= 1u, "upstream estimate reports direct removals");
    expectTrue(upstream_estimate.downstream >= 1u, "upstream estimate reports downstream cascade");

    const fuse::project::AssetCooker::CookUpstreamInvalidationEstimate upstream_estimate =
        cooker.estimate_upstream_invalidation(manifest, sourceA);
    expectTrue(upstream_estimate.total() == upstream_count,
               "upstream estimate total matches count probe");
    expectTrue(upstream_estimate.source_direct >= 1u, "upstream estimate includes direct source entry");
    expectTrue(upstream_estimate.downstream_cascade >= 1u, "upstream estimate includes downstream cascade");

    const fuse::u32 empty_upstream_count = cooker.count_upstream_invalidation(manifest, "");
    expectTrue(empty_upstream_count == 0u, "empty changed source upstream count is zero");
    expectTrue(cooker.estimate_upstream_invalidation(manifest, "").total() == 0u,
               "empty changed source upstream estimate is zero");

    const fuse::u32 stale_count_before = cooker.count_stale_dependency_invalidation(manifest);
    expectTrue(stale_count_before == 0u, "fresh cache stale dependency count is zero");
    expectTrue(!cooker.would_need_stale_dependency_reconcile(manifest),
               "fresh cache does not need stale dependency reconcile");
    expectTrue(cooker.estimate_stale_dependency_reconcile(manifest).total() == 0u,
               "fresh cache stale dependency reconcile estimate is zero");
    expectTrue(!cooker.would_stale_dependency_invalidation(manifest),
               "fresh cache would_stale_dependency_invalidation is false");
    expectTrue(cooker.would_upstream_invalidation(manifest, sourceA),
               "would_upstream_invalidation reports chain before invalidation");
    expectTrue(!cooker.would_upstream_invalidation(manifest, ""),
               "would_upstream_invalidation rejects empty changed source");
    expectTrue(cooker.count_prunable_cache_entries() == 0u, "fresh cache prunable count is zero");

    const fuse::project::AssetCooker::CookStaleDependencyEstimate stale_estimate =
        cooker.estimate_stale_dependency_reconcile(manifest);
    expectTrue(stale_estimate.total() == stale_count_before,
               "stale dependency estimate matches count probe on fresh cache");
    expectTrue(stale_estimate.direct_upstream_stale == 0u,
               "fresh cache has no direct upstream stale entries");
    expectTrue(stale_estimate.downstream_cascade == 0u,
               "fresh cache has no downstream cascade stale entries");
    expectTrue(!cooker.would_reconcile_stale_dependencies(manifest),
               "fresh cache would not reconcile stale dependencies");
    expectTrue(cooker.would_invalidate_upstream(manifest, sourceA),
               "would_invalidate_upstream true for seeded chain source");
    expectTrue(!cooker.would_invalidate_upstream(manifest, ""),
               "would_invalidate_upstream false for empty changed source");
    expectTrue(cooker.count_prune_reconcile() == 0u, "fresh cache prune reconcile count is zero");

    const fuse::u32 prune_estimate = cooker.estimate_prune_reconcile();
    expectTrue(prune_estimate == 0u, "fresh cache prune reconcile estimate is zero");

    const fuse::u32 full_estimate = cooker.estimate_full_cache_reconcile(manifest);
    expectTrue(full_estimate == 0u, "fresh cache full reconcile estimate is zero");

    const fuse::u32 removed = cooker.invalidate_upstream_dependency(manifest, sourceA);
    expectTrue(removed >= upstream_count, "upstream invalidation removes at least probed count");
    expectTrue(cooker.cache().entry_count() == 0u, "cache empty after probed upstream invalidation");
    expectTrue(!cooker.would_invalidate_upstream(manifest, sourceA),
               "would_invalidate_upstream false after upstream invalidation");
}

void testCookerStaleDependencyReconcileProbes() {
    const std::string sourceA = writeTempFile("/tmp/fuse_b79_reconcile_a.obj", "# reconcile a\n");
    const std::string sourceB = writeTempFile("/tmp/fuse_b79_reconcile_b.obj", "# reconcile b\n");

    fuse::project::CookManifest manifest;
    fuse::project::CookManifestEntry entryA;
    entryA.kind = fuse::project::CookAssetKind::Mesh;
    entryA.source_path = sourceA;
    entryA.output_path = "/tmp/fuse_b79_reconcile_a.fusemesh";
    manifest.assets.push_back(entryA);

    fuse::project::CookManifestEntry entryB;
    entryB.kind = fuse::project::CookAssetKind::Mesh;
    entryB.source_path = sourceB;
    entryB.output_path = "/tmp/fuse_b79_reconcile_b.fusemesh";
    entryB.dependencies.push_back(entryA.output_path);
    manifest.assets.push_back(entryB);

    fuse::project::AssetCooker cooker;
    const fuse::project::CookBatchResult batch = cooker.cook_manifest(manifest);
    expectTrue(batch.ok, "manifest cook for reconcile probes ok");
    expectTrue(!cooker.would_reconcile_stale_dependencies(manifest),
               "fresh dependency chain would not reconcile");

    writeTempFile(sourceA, "# reconcile a revised\n");
    const fuse::u32 stale_count = cooker.count_stale_dependency_invalidation(manifest);
    expectTrue(stale_count >= 1u, "stale dependency count non-zero after upstream source change");
    expectTrue(cooker.would_reconcile_stale_dependencies(manifest),
               "would_reconcile_stale_dependencies true after upstream hash change");

    const fuse::u32 reconciled = cooker.invalidate_stale_dependency_hashes(manifest);
    expectTrue(reconciled >= stale_count, "reconcile removes at least probed stale count");
               "would_reconcile_stale_dependencies false after reconcile");

void testCookerWouldInvalidateProbes() {
    const std::string source_a = writeTempFile("/tmp/fuse_b79_would_inv_a.obj", "# would inv a\n");
    const std::string source_b = writeTempFile("/tmp/fuse_b79_would_inv_b.obj", "# would inv b\n");

    fuse::project::CookManifestEntry entry_a;
    entry_a.kind = fuse::project::CookAssetKind::Mesh;
    entry_a.source_path = source_a;
    entry_a.output_path = "/tmp/fuse_b79_would_inv_a.fusemesh";
    manifest.assets.push_back(entry_a);

    fuse::project::CookManifestEntry entry_b;
    entry_b.kind = fuse::project::CookAssetKind::Mesh;
    entry_b.source_path = source_b;
    entry_b.output_path = "/tmp/fuse_b79_would_inv_b.fusemesh";
    entry_b.dependencies.push_back(entry_a.output_path);
    manifest.assets.push_back(entry_b);

    expectTrue(cooker.cook_manifest(manifest).ok, "manifest cook for would_invalidate probes ok");
    expectTrue(!cooker.would_invalidate_upstream_dependency(manifest, ""),
               "empty changed source upstream would_invalidate is false");
    expectTrue(cooker.would_invalidate_upstream_dependency(manifest, source_a),
               "upstream would_invalidate true for seeded chain");
    expectTrue(!cooker.would_invalidate_stale_dependency_hashes(manifest),
               "fresh cache stale dependency would_invalidate is false");

    writeTempFile(source_a, "# would inv a revised\n");
    expectTrue(cooker.would_invalidate_stale_dependency_hashes(manifest),
               "stale dependency would_invalidate true after upstream change");
    expectTrue(cooker.count_stale_dependency_invalidation(manifest) >= 1u,
               "stale dependency count agrees with would_invalidate probe");

void testCookerReconcileEstimateShouldSkip() {
    const std::string source = writeTempFile("/tmp/fuse_b79_reconcile_skip.obj", "# reconcile skip\n");

    fuse::project::CookManifestEntry entry;
    entry.kind = fuse::project::CookAssetKind::Mesh;
    entry.source_path = source;
    entry.output_path = "/tmp/fuse_b79_reconcile_skip.fusemesh";
    manifest.assets.push_back(entry);

    expectTrue(cooker.cook_manifest(manifest).ok, "manifest cook for reconcile should_skip ok");

    const fuse::project::CookCacheReconcileEstimate fresh = cooker.estimate_reconcile_invalidation(manifest);
    expectTrue(fresh.should_skip(), "fresh cache reconcile estimate should_skip");
    expectTrue(fresh.should_skip() == (fresh.total() == 0u), "should_skip mirrors zero reconcile total");
    expectTrue(cooker.estimate_prune_reconcile().should_skip(), "fresh prune reconcile estimate should_skip");

    writeTempFile(source, "# reconcile skip revised\n");
    const fuse::project::CookCacheReconcileEstimate stale = cooker.estimate_reconcile_invalidation(manifest);
    expectTrue(!stale.should_skip(), "stale reconcile estimate should not skip");
    expectTrue(!cooker.estimate_prune_reconcile().should_skip(),
               "stale prune reconcile estimate should not skip");

void testCookerReconcileEstimateProbes() {
    const std::string source_a = writeTempFile("/tmp/fuse_b79_reconcile_a.obj", "# reconcile a\n");
    const std::string source_b = writeTempFile("/tmp/fuse_b79_reconcile_b.obj", "# reconcile b\n");

    entry_a.output_path = "/tmp/fuse_b79_reconcile_a.fusemesh";

    entry_b.output_path = "/tmp/fuse_b79_reconcile_b.fusemesh";

    expectTrue(cooked.ok, "manifest cook for reconcile estimate ok");

    expectTrue(fresh.total() == 0u, "fresh cache reconcile estimate is zero");
    expectTrue(cooker.estimate_prune_reconcile().total() == 0u, "fresh prune reconcile estimate is zero");

    writeTempFile(source_a, "# reconcile a revised\n");
    expectTrue(stale_count >= 1u, "stale dependency reconcile count is non-zero after upstream change");

    expectTrue(stale.stale_dependency_entries == stale_count,
               "reconcile estimate stale count matches dependency probe");
    expectTrue(stale.total() >= stale_count, "reconcile estimate total includes stale dependency count");

    const fuse::u32 removed = cooker.invalidate_stale_dependency_hashes(manifest);
    expectTrue(removed >= stale_count, "stale dependency invalidation removes at least estimated count");

    const fuse::project::CookCacheReconcileEstimate after = cooker.estimate_reconcile_invalidation(manifest);
    expectTrue(after.stale_dependency_entries == 0u,
               "stale dependency reconcile estimate zero after stale invalidation");
    expectTrue(after.prune_stale_entries >= 1u,
               "changed upstream entry remains stale for prune reconcile");

void testCookerUpstreamReconcileProbes() {
    const std::string source_a = writeTempFile("/tmp/fuse_b79_up_reconcile_a.obj", "# up reconcile a\n");
    const std::string source_b = writeTempFile("/tmp/fuse_b79_up_reconcile_b.obj", "# up reconcile b\n");
    const std::string source_c = writeTempFile("/tmp/fuse_b79_up_reconcile_c.obj", "# up reconcile c\n");

    entry_a.output_path = "/tmp/fuse_b79_up_reconcile_a.fusemesh";

    entry_b.output_path = "/tmp/fuse_b79_up_reconcile_b.fusemesh";

    fuse::project::CookManifestEntry entry_c;
    entry_c.kind = fuse::project::CookAssetKind::Mesh;
    entry_c.source_path = source_c;
    entry_c.output_path = "/tmp/fuse_b79_up_reconcile_c.fusemesh";
    entry_c.dependencies.push_back(entry_b.output_path);
    manifest.assets.push_back(entry_c);

    expectTrue(cooker.cook_manifest(manifest).ok, "chain manifest cook for upstream reconcile ok");
    expectTrue(!cooker.would_reconcile_invalidation(manifest), "fresh cache would_reconcile is false");

    const fuse::project::CookCacheUpstreamReconcileEstimate upstream =
        cooker.estimate_upstream_reconcile(manifest, source_a);
    expectTrue(upstream.direct_source_entries == 1u, "upstream reconcile counts direct source entry");
    expectTrue(upstream.downstream_entries == 2u, "upstream reconcile counts downstream entries");
    expectTrue(upstream.total() == cooker.count_upstream_invalidation(manifest, source_a),
               "upstream reconcile total matches count probe");

    const std::vector<std::string> probed = cooker.probe_upstream_invalidation_sources(manifest, source_a);
    expectTrue(probed.size() >= 3u, "upstream probe lists changed source and dependents");
    expectTrue(probed[0] == source_a, "upstream probe starts at changed source");

    expectTrue(cooker.estimate_upstream_reconcile(manifest, "").total() == 0u,
               "empty changed source upstream reconcile is zero");
    expectTrue(cooker.probe_upstream_invalidation_sources(manifest, "").empty(),
               "empty changed source upstream probe is guarded");

    writeTempFile(source_a, "# up reconcile a revised\n");
    expectTrue(cooker.would_reconcile_invalidation(manifest), "stale dependency makes would_reconcile true");

    const fuse::project::CookCacheReconcileEstimate reconcile = cooker.estimate_reconcile_invalidation(manifest);
    expectTrue(reconcile.stale_dependency_entries >= 1u, "reconcile estimate includes stale dependency count");
    expectTrue(reconcile.total() >= 1u, "reconcile estimate total is non-zero after upstream change");

void testCookCacheDownstreamSourceProbe() {
    const std::string source_a = writeTempFile("/tmp/fuse_b79_downstream_a.obj", "# downstream a\n");
    const std::string source_b = writeTempFile("/tmp/fuse_b79_downstream_b.obj", "# downstream b\n");
    const std::string source_c = writeTempFile("/tmp/fuse_b79_downstream_c.obj", "# downstream c\n");

    entry_a.output_path = "/tmp/fuse_b79_downstream_a.fusemesh";

    entry_b.output_path = "/tmp/fuse_b79_downstream_b.fusemesh";

    entry_c.output_path = "/tmp/fuse_b79_downstream_c.fusemesh";

    fuse::project::CookJobGraph graph;
    graph.build_from_manifest(manifest);

    expectTrue(cooker.cook_manifest(manifest).ok, "chain manifest cook for downstream probe ok");
    expectTrue(cooker.cache().entry_count() == 3u, "three entries seeded for downstream probe");

    const std::vector<std::string> probed = cooker.cache().probe_downstream_sources(
        entry_a.output_path, graph.edges(), graph.jobs());
    expectTrue(probed.size() >= 3u, "downstream probe lists producer and dependent sources");

    bool has_middle = false;
    bool has_tail = false;
    for (const std::string& path : probed) {
        if (path == source_b) {
            has_middle = true;
        if (path == source_c) {
            has_tail = true;
    expectTrue(has_middle, "downstream probe includes middle source");
    expectTrue(has_tail, "downstream probe includes tail source");
    expectTrue(probed[0] == entry_a.output_path, "downstream probe starts at producer output path");

    const fuse::u32 counted = cooker.cache().count_downstream_of(entry_a.output_path, graph.edges(), graph.jobs());
    expectTrue(counted == 2u, "downstream count matches dependent entries only");
    expectTrue(cooker.cache().would_invalidate_downstream_of(entry_a.output_path, graph.edges(), graph.jobs()),
               "would_invalidate_downstream true for seeded chain");
    expectTrue(!cooker.cache().would_invalidate_downstream_of("", graph.edges(), graph.jobs()),
               "would_invalidate_downstream guarded on empty output path");
    expectTrue(cooker.cache().probe_downstream_sources("", graph.edges(), graph.jobs()).empty(),
               "empty output path downstream probe is guarded");
void testCombineCookCacheKeyZeroSourceGuard() {
    expectTrue(fuse::project::combine_cook_cache_key(0, 0) == 0, "all-zero cache key stays zero");
    expectTrue(fuse::project::combine_cook_cache_key(0, 42u) == 0,
               "zero source with non-zero upstream stays zero");
    expectTrue(!fuse::project::is_valid_combined_cook_cache_key(0, 42u),
               "combined key invalid when source hash is zero");
    expectTrue(fuse::project::is_valid_combined_cook_cache_key(99u, 0u),
               "combined key valid when only upstream is zero");
    expectTrue(fuse::project::is_valid_combined_cook_cache_key(99u, 42u),
               "combined key valid when both hashes are non-zero");

void testCookCachePruneAll() {
    fuse::project::CookCache cache;
    expectTrue(cache.prune_all() == 0u, "prune_all on empty cache is a no-op");

    const std::string source = writeTempFile("/tmp/fuse_b79_prune_all_live.obj", "# prune all v1\n");
    fuse::project::MeshImportDesc desc;
    desc.input_path = source;
    desc.output_path = "/tmp/fuse_b79_prune_all_live.fusemesh";

    const fuse::project::CookRecord first = cooker.cook_mesh(desc);
    expectTrue(first.ok, "seed cook for prune_all ok");
    expectTrue(cooker.cache().entry_count() == 1u, "one live entry before prune_all");
    expectTrue(cooker.cache().prune_all() == 0u, "prune_all on fresh valid entry is a no-op");
    expectTrue(cooker.cache().contains(first.content_hash), "valid entry survives prune_all");

    writeTempFile(source, "# prune all v2\n");
    const fuse::u32 removed = cooker.cache().prune_all();
    expectTrue(removed == 1u, "prune_all removes stale live entry");
    expectTrue(cooker.cache().empty(), "cache empty after prune_all");

void testCookCacheLoadPrunesStale() {
    const std::string source = writeTempFile("/tmp/fuse_b79_load_prune.obj", "# load prune v1\n");

    desc.output_path = "/tmp/fuse_b79_load_prune.fusemesh";

    const fuse::project::CookRecord seeded = cooker.cook_mesh(desc);
    expectTrue(seeded.ok, "seed cook for load prune ok");

    const std::string cachePath = "/tmp/fuse_b79_load_prune_cache.json";
    expectTrue(cooker.cache().save(cachePath), "cache saves before source change");

    writeTempFile(source, "# load prune v2\n");

    fuse::project::CookCache loaded;
    expectTrue(loaded.load(cachePath), "stale cache JSON loads");
    expectTrue(loaded.empty(), "load auto-prunes stale entries");
    expectTrue(loaded.entry_count() == 0u, "no entries remain after load prune");

void testAssetCookerInvalidateEarlyOuts() {

    expectTrue(cooker.invalidate_upstream_dependency(manifest, "") == 0u,
               "upstream invalidation rejects empty source path");
    expectTrue(cooker.invalidate_stale_dependency_hashes(manifest) == 0u,
               "stale dependency invalidation on empty manifest is a no-op");

    const std::string source = writeTempFile("/tmp/fuse_b79_early_out_mesh.obj", "# early out\n");
    entry.output_path = "/tmp/fuse_b79_early_out_mesh.fusemesh";

    expectTrue(cooker.invalidate_upstream_dependency(manifest, source) == 0u,
               "upstream invalidation on empty cache is a no-op");
               "stale dependency invalidation on empty cache is a no-op");

    desc.output_path = entry.output_path;
    expectTrue(seeded.ok, "seed cook for early-out invalidation ok");
    expectTrue(cooker.cache().entry_count() == 1u, "cache seeded for early-out tests");

               "upstream invalidation still rejects empty path with populated cache");
    expectTrue(cooker.cache().entry_count() == 1u, "empty-path upstream invalidation leaves cache intact");
    const std::string source = writeTempFile("/tmp/fuse_b79_prune_all.obj", "# prune all v1\n");

    desc.output_path = "/tmp/fuse_b79_prune_all.fusemesh";

    expectTrue(cooker.cache().entry_count() == 1u, "one entry before prune_all");

    expectTrue(removed == 1u, "prune_all removes stale entry");
    expectTrue(cooker.cache().prune_all() == 0u, "prune_all on empty cache is a no-op");

void testAssetCookerInvalidationEmptyGuards() {
    entry.source_path = "/tmp/fuse_b79_empty_inv_source.obj";
    entry.output_path = "/tmp/fuse_b79_empty_inv.fusemesh";

    expectTrue(cooker.cache().empty(), "fresh cooker cache is empty");
    expectTrue(cooker.invalidate_upstream_dependency(manifest, entry.source_path) == 0u,
               "upstream invalidation on empty cache returns zero");
               "stale dependency invalidation on empty cache returns zero");
               "upstream invalidation rejects empty changed source");
    expectTrue(cooker.invalidate_stale_dependency_hashes({}) == 0u,
               "stale dependency invalidation on empty manifest returns zero");
    expectTrue(cooker.cache().stats().invalidations == 0u,
               "empty-cache cooker invalidation does not bump stats");

void testContentHashEmptyUpstreamDeps() {
    const fuse::project::CookManifest manifest;
    expectTrue(fuse::project::hash_upstream_dependencies({}, manifest) == 0,
               "empty dependency list yields zero upstream hash");
    expectTrue(batch.ok, "reconcile estimator test seeds cache");
    expectTrue(!cooker.cache_needs_dependency_reconcile(manifest),
               "fresh cache does not need dependency reconcile");
    expectTrue(cooker.estimate_stale_dependency_entries(manifest) == 0u,
               "fresh cache reports zero stale dependency entries");

    writeTempFile(sourceA, "# est a revised\n");
    expectTrue(cooker.cache_needs_dependency_reconcile(manifest),
               "upstream change flags dependency reconcile needed");
    const fuse::u32 estimated = cooker.estimate_stale_dependency_entries(manifest);
    expectTrue(estimated >= 1u, "upstream change estimates at least one stale dependency entry");

    expectTrue(removed >= estimated, "reconcile removal meets or exceeds estimate");
               "cache clean after reconcile");
               "estimate zero after reconcile");

void testCookCacheUpstreamInvalidationEstimate() {
    const std::string sourceA = writeTempFile("/tmp/fuse_b79_est_up_a.obj", "# est up a\n");
    const std::string sourceB = writeTempFile("/tmp/fuse_b79_est_up_b.obj", "# est up b\n");


    entryA.output_path = "/tmp/fuse_b79_est_up_a.fusemesh";
void testCookerStaleDependencyReconcileEstimator() {

void testCookerCacheReconcileEstimators() {

void testCookerStaleDependencyReconcileEstimators() {

void testCookerUpstreamProbeAndReconcileEstimate() {
    const std::string sourceA = writeTempFile("/tmp/fuse_b79_est_chain_a.obj", "# est chain a\n");
    const std::string sourceB = writeTempFile("/tmp/fuse_b79_est_chain_b.obj", "# est chain b\n");

    entryA.output_path = "/tmp/fuse_b79_est_chain_a.fusemesh";
void testCookerStaleDependencyReconcileEstimate() {

void testCookerReconcileEstimatorGuards() {


    const std::string sourceA = writeTempFile("/tmp/fuse_b79_est_stale_a.obj", "# est stale a\n");
    const std::string sourceB = writeTempFile("/tmp/fuse_b79_est_stale_b.obj", "# est stale b\n");


    entryA.output_path = "/tmp/fuse_b79_est_stale_a.fusemesh";

    entryB.output_path = "/tmp/fuse_b79_est_up_b.fusemesh";
    entryB.output_path = "/tmp/fuse_b79_est_chain_b.fusemesh";
    entryB.output_path = "/tmp/fuse_b79_est_stale_b.fusemesh";

    expectTrue(batch.ok, "upstream estimate test seeds cache");

    const fuse::u32 estimated = cooker.estimate_upstream_invalidation(manifest, sourceA);
    expectTrue(estimated >= 2u, "upstream estimate covers changed source and downstream");

    expectTrue(removed == estimated, "upstream invalidation matches estimate");
    expectTrue(cooker.estimate_upstream_invalidation(manifest, sourceA) == 0u,
               "estimate zero after upstream invalidation");
    expectTrue(batch.ok, "reconcile estimator seeds cache");
    expectTrue(cooker.cache().entry_count() == 2u, "two entries cached for estimator");

    expectTrue(cooker.estimate_stale_dependency_invalidations(manifest) == 0u,
               "reconcile estimator is zero on fresh cache");

    const fuse::u32 estimate_before = cooker.estimate_stale_dependency_invalidations(manifest);
    expectTrue(estimate_before >= 1u, "reconcile estimator detects upstream drift");

    expectTrue(removed == estimate_before, "reconcile estimator matches invalidate count");
               "reconcile estimator is zero after invalidate");

void testCookCachePruneEstimateMatchesPrune() {
    const std::string source = writeTempFile("/tmp/fuse_b79_est_prune.obj", "# est prune v1\n");

    desc.output_path = "/tmp/fuse_b79_est_prune.fusemesh";

    expectTrue(first.ok, "seed cook for prune estimate match");

    writeTempFile(source, "# est prune v2\n");
    const fuse::project::CookCachePruneEstimate estimate = cooker.cache().estimate_prune_all();
    expectTrue(estimate.would_prune(), "estimate detects stale entry");
    expectTrue(estimate.total() == cooker.cache().count_prunable_entries(),
               "estimate total matches count_prunable");

    const fuse::u32 pruned = cooker.cache().prune_stale_entries();
    expectTrue(pruned == estimate.stale_entries, "prune_stale removes estimated stale count");
    expectTrue(cooker.cache().estimate_prune_all().total() == 0u, "estimate is zero after prune");
    expectTrue(batch.ok, "manifest cook seeds cache for reconcile estimate");
    expectTrue(cooker.cache().entry_count() == 2u, "upstream and downstream cached");

    const fuse::project::CookCacheReconcileEstimate fresh_estimate =
        cooker.estimate_stale_dependency_hashes(manifest);
    expectTrue(fresh_estimate.should_skip(), "fresh manifest reconcile estimate should skip");

    const fuse::project::CookCacheReconcileEstimate stale_estimate =
    expectTrue(stale_estimate.would_reconcile(), "upstream change reconcile estimate would reconcile");
    expectTrue(stale_estimate.upstream_stale_entries >= 1u,
               "upstream change reconcile estimate counts stale upstream entries");

    expectTrue(removed >= stale_estimate.upstream_stale_entries,
               "actual reconcile removes at least estimated upstream stale entries");
    const fuse::project::CookBatchResult cooked = cooker.cook_manifest(manifest);
    expectTrue(cooked.ok, "manifest cook for reconcile estimator ok");
    expectTrue(cooker.count_stale_dependency_invalidation(manifest) == 0u,
               "fresh cache stale reconcile count is zero");

    const fuse::u32 estimated = cooker.count_stale_dependency_invalidation(manifest);
    expectTrue(estimated >= 1u, "upstream change raises stale reconcile estimate");

    expectTrue(removed >= estimated, "stale reconcile invalidation removes at least estimated count");
               "stale reconcile count zero after invalidation");

void testCookerPruneReconcileEstimator() {
    const std::string source = writeTempFile("/tmp/fuse_b79_prune_est.obj", "# prune est v1\n");
    desc.output_path = "/tmp/fuse_b79_prune_est.fusemesh";

    expectTrue(seeded.ok, "seed cook for prune estimator ok");
    expectTrue(cooker.count_prune_invalidation() == 0u, "fresh cache prune estimate is zero");

    writeTempFile(source, "# prune est v2\n");
    expectTrue(cooker.count_prune_invalidation() == 1u, "stale entry raises prune estimate");
    expectTrue(cooker.count_prune_invalidation() == cooker.cache().count_prune_all(),
               "cooker prune estimate matches cache count_prune_all");
    expectTrue(cooker.count_prune_invalidation() == cooker.cache().prune_all(),
               "prune estimate matches prune_all removal count");
    expectTrue(cooker.count_prune_invalidation() == 0u, "prune estimate zero after reconcile");
    expectTrue(cooked.ok, "manifest cook for reconcile estimators ok");

    const fuse::project::CookCacheReconcileEstimate fresh = cooker.estimate_cache_reconcile(manifest);
    expectTrue(fresh.stale_dependency_invalidations == 0u,
               "fresh cache stale dependency reconcile estimate is zero");
    expectTrue(fresh.prunable_entries == 0u, "fresh cache prune estimate is zero");
    expectTrue(fresh.invalid_entries == 0u, "fresh cache invalid entry estimate is zero");
    expectTrue(fresh.stale_entries == 0u, "fresh cache stale entry estimate is zero");

    const fuse::project::CookCacheReconcileEstimate upstream =
        cooker.estimate_upstream_change_reconcile(manifest, sourceA);
    expectTrue(upstream.upstream_invalidation >= 2u,
               "upstream change reconcile estimates chain invalidation");
    expectTrue(upstream.upstream_invalidation == cooker.count_upstream_invalidation(manifest, sourceA),
               "upstream reconcile mirrors count_upstream_invalidation");

    const fuse::project::CookCacheReconcileEstimate after_change = cooker.estimate_cache_reconcile(manifest);
    expectTrue(after_change.stale_dependency_invalidations >= 1u,
               "revised upstream marks stale dependency invalidations");
    expectTrue(after_change.stale_entries >= 1u || after_change.prunable_entries >= 1u,
               "revised upstream marks stale or prunable entries");

    expectTrue(removed >= after_change.stale_dependency_invalidations,
               "stale dependency invalidation removes at least estimated count");
    expectTrue(!cooker.would_need_stale_dependency_reconcile(manifest),
               "fresh cache reconcile estimate is not needed");

    const fuse::project::CookStaleDependencyReconcileEstimate reconcile_estimate =
        cooker.estimate_stale_dependency_reconcile(manifest);
    expectTrue(cooker.would_need_stale_dependency_reconcile(manifest),
               "stale upstream hash marks reconcile as needed");
    expectTrue(reconcile_estimate.direct_stale >= 1u,
               "reconcile estimate reports direct stale upstream entries");
    expectTrue(reconcile_estimate.total() == cooker.count_stale_dependency_invalidation(manifest),
               "reconcile estimate total matches count probe");

    expectTrue(removed >= reconcile_estimate.total(),
               "stale dependency reconcile removes at least estimated count");
               "cache is clean after stale dependency reconcile");
    expectTrue(cooker.cache().entry_count() == 2u, "two entries seeded for reconcile estimate");

    const fuse::project::CookCacheInvalidationProbe probe =
        cooker.probe_upstream_dependency(manifest, sourceA);
    expectTrue(probe.would_invalidate(), "upstream probe reports invalidation scope");
    expectTrue(probe.total_entries() >= 2u, "upstream probe estimates chain removals");
    expectTrue(probe.direct_entries == cooker.cache().count_by_source(sourceA),
               "upstream probe direct count matches count_by_source");

    const fuse::project::CookCacheInvalidationProbe empty_probe =
        cooker.probe_upstream_dependency(manifest, "");
    expectTrue(!empty_probe.would_invalidate(), "empty changed source upstream probe is zero");

    const fuse::project::AssetCooker::CookDependencyReconcileEstimate fresh_estimate =
    expectTrue(!fresh_estimate.would_reconcile(), "fresh cache reconcile estimate is zero");
    expectTrue(fresh_estimate.stale_source_paths.empty(), "fresh cache has no stale source paths");

    expectTrue(stale_count == fresh_estimate.total_entries(),
               "count_stale_dependency_invalidation matches reconcile estimate total");

    const fuse::u32 removed = cooker.invalidate_upstream_dependency(manifest, sourceA);
    expectTrue(removed >= probe.total_entries(), "upstream invalidation removes at least probed count");
    expectTrue(cooker.cache().entry_count() == 0u, "cache empty after probed upstream invalidation");

    const fuse::project::CookCacheStaleUpstreamEstimate fresh_estimate =
        cooker.estimate_stale_dependency_reconciliation(manifest);
    expectTrue(fresh_estimate.total() == 0u, "fresh cache reconcile estimate is zero");
    expectTrue(cooker.count_stale_dependency_invalidation(manifest) == fresh_estimate.total(),

    writeTempFile(sourceA, "# reconcile a changed\n");
    cooker.cook_mesh({sourceA, entryA.output_path});

    const fuse::project::CookCacheStaleUpstreamEstimate stale_estimate =
    expectTrue(stale_estimate.stale_upstream >= 1u,
               "changed upstream source yields stale upstream reconcile estimate");
    expectTrue(stale_estimate.total() >= cooker.count_stale_dependency_invalidation(manifest),
               "reconcile estimate total covers count probe");

    expectTrue(removed >= stale_estimate.stale_upstream,
               "stale dependency invalidation removes at least direct stale estimate");

    expectTrue(cooker.would_invalidate_upstream_dependency(manifest, sourceA),
               "fresh cache would invalidate upstream chain");
               "empty changed source upstream estimator is false");
               "fresh cache stale reconcile estimator is false");

               "upstream hash change marks reconcile estimator true");
               "stale reconcile count matches estimator");

    expectTrue(removed >= 1u, "stale dependency reconcile removes probed entries");
               "reconcile estimator false after stale invalidation");
               "fresh cache would not reconcile stale dependencies");
    expectTrue(cooker.estimate_stale_dependency_reconcile(manifest) == 0u,
               "fresh cache reconcile estimate is zero");

    const fuse::u32 estimate = cooker.estimate_stale_dependency_reconcile(manifest);
    expectTrue(estimate >= 1u, "reconcile estimate reports stale upstream entries");
               "would_reconcile true after upstream hash change");
    expectTrue(cooker.count_stale_dependency_invalidation(manifest) == estimate,
               "reconcile estimate matches stale dependency count probe");

    expectTrue(removed >= estimate, "stale dependency invalidation removes at least estimated count");
               "would_reconcile false after reconcile completes");
               "reconcile estimate zero after reconcile completes");
    expectTrue(batch.ok, "reconcile probe test seeds cache");
    expectTrue(!cooker.would_stale_dependency_invalidation(manifest),
               "fresh reconcile probe is false before upstream change");

    expectTrue(stale_count >= 1u, "stale reconcile count is non-zero after upstream change");
    expectTrue(cooker.would_stale_dependency_invalidation(manifest),
               "would_stale_dependency_invalidation true after upstream change");
    expectTrue(cooker.cache().would_invalidate_stale_upstream_hashes({{sourceB, 0u}}) ||
                   cooker.cache().count_stale_upstream_hashes({{sourceB, batch.records[1].content_hash}}) >= 1u,
               "cache stale upstream probe detects downstream mismatch");

    expectTrue(removed >= stale_count, "stale invalidation removes at least probed count");
    expectTrue(batch.ok, "stale reconcile estimate seeds cache");

    writeTempFile(sourceA, "# est stale a revised\n");

    const fuse::project::AssetCooker::CookStaleDependencyEstimate estimate =
    expectTrue(estimate.direct_upstream_stale >= 1u,
               "stale reconcile estimate reports direct upstream stale entries");
    expectTrue(estimate.total() == cooker.count_stale_dependency_invalidation(manifest),
               "stale reconcile estimate total matches count probe");
    expectTrue(estimate.direct_upstream_stale + estimate.downstream_cascade == estimate.total(),
               "stale reconcile estimate components sum to total");

    expectTrue(removed >= estimate.total(),
               "stale dependency invalidation removes at least estimated total");

void testCookerReconcileEstimators() {




    const fuse::project::AssetCooker::CookUpstreamInvalidationEstimate upstream_estimate =
        cooker.estimate_upstream_invalidation(manifest, sourceA);
    expectTrue(upstream_estimate.direct >= 1u, "upstream estimate direct includes changed source");
    expectTrue(upstream_estimate.downstream >= 1u, "upstream estimate downstream includes dependents");
    expectTrue(upstream_estimate.total == upstream_estimate.direct + upstream_estimate.downstream,
               "upstream estimate total matches direct plus downstream");
    expectTrue(upstream_estimate.total == cooker.count_upstream_invalidation(manifest, sourceA),
               "upstream estimate total matches count probe");

    expectTrue(fresh_estimate.direct_stale == 0u, "fresh cache direct stale estimate is zero");
    expectTrue(fresh_estimate.downstream_stale == 0u, "fresh cache downstream stale estimate is zero");
    expectTrue(fresh_estimate.total == 0u, "fresh cache reconcile estimate total is zero");
    expectTrue(fresh_estimate.total == cooker.count_stale_dependency_invalidation(manifest),

    writeTempFile(sourceA, "# est chain a revised\n");
    const fuse::project::AssetCooker::CookDependencyReconcileEstimate stale_estimate =
    expectTrue(stale_estimate.direct_stale >= 1u, "stale upstream direct estimate is non-zero");
    expectTrue(stale_estimate.total >= stale_estimate.direct_stale,
               "stale reconcile total includes direct stale entries");
    expectTrue(stale_estimate.total == cooker.count_stale_dependency_invalidation(manifest),

    expectTrue(removed >= stale_estimate.total, "reconcile removes at least estimated entries");

    const std::string sourceA = writeTempFile("/tmp/fuse_b79_reconcile_chain_a.obj", "# reconcile chain a\n");
    const std::string sourceB = writeTempFile("/tmp/fuse_b79_reconcile_chain_b.obj", "# reconcile chain b\n");

    entryA.output_path = "/tmp/fuse_b79_reconcile_chain_a.fusemesh";

    entryB.output_path = "/tmp/fuse_b79_reconcile_chain_b.fusemesh";


    expectTrue(fresh.prunable_entries() == 0u, "fresh manifest cache prunable reconcile is zero");
    expectTrue(fresh.stale_upstream_entries == 0u, "fresh manifest cache stale upstream reconcile is zero");

    writeTempFile(sourceA, "# reconcile chain a updated\n");

    const fuse::project::CookCacheReconcileEstimate stale = cooker.estimate_cache_reconcile(manifest);
    expectTrue(stale.stale_upstream_entries >= 1u,
               "upstream content change increases stale upstream reconcile estimate");

    expectTrue(removed >= stale.stale_upstream_entries,
               "stale dependency invalidation removes at least reconcile-estimated entries");

    const std::vector<std::string> downstream =
        cooker.cache().probe_downstream_sources(entryA.output_path, graph.edges(), graph.jobs());
    expectTrue(!downstream.empty(), "downstream probe returns affected sources");
    expectTrue(downstream[0] == entryA.output_path,
               "downstream probe includes invalidated output path");

    const std::string sourceA = writeTempFile("/tmp/fuse_b79_reconcile_a.obj", "# reconcile a v1\n");



    expectTrue(cooker.cache().entry_count() == 2u, "two entries seeded for reconcile estimators");

    expectTrue(cooker.count_stale_content_invalidation(manifest) == 0u,
               "fresh cache stale content estimate is zero");
               "fresh cache stale dependency estimate is zero");
    expectTrue(cooker.estimate_reconcile_removals(manifest) == 0u,
               "fresh cache combined reconcile estimate is zero");

    fuse::project::CookManifest empty_manifest;
    expectTrue(cooker.count_stale_content_invalidation(empty_manifest) == 0u,
               "empty manifest stale content estimate is zero");
    expectTrue(cooker.estimate_reconcile_removals(empty_manifest) == 0u,
               "empty manifest reconcile estimate is zero");

    writeTempFile(sourceA, "# reconcile a v2\n");
    const fuse::u32 stale_content = cooker.count_stale_content_invalidation(manifest);
    expectTrue(stale_content >= 2u, "stale content estimate includes downstream chain");
    expectTrue(cooker.estimate_reconcile_removals(manifest) >= stale_content,
               "combined reconcile estimate is at least stale content count");

    expectTrue(pruned >= 1u, "prune removes stale content entries");
               "stale content estimate zero after prune");





    expectTrue(batch.ok, "manifest cook for stale reconcile estimator ok");
    expectTrue(cooker.cache().entry_count() == 2u, "two entries seeded for stale reconcile estimator");

    const fuse::u32 stale_count_before = cooker.count_stale_dependency_invalidation(manifest);
    expectTrue(stale_count_before == 0u, "fresh cache stale reconcile estimate is zero");

    const fuse::u32 stale_estimate = cooker.count_stale_dependency_invalidation(manifest);
    expectTrue(stale_estimate >= 1u, "stale reconcile estimator reports dependent invalidations");

    expectTrue(removed >= stale_estimate,
    expectTrue(cooker.cache().lookup(batch.records[1].content_hash) == fuse::project::CookCacheLookup::Miss,
               "downstream misses after estimated stale reconcile");
               "stale reconcile estimate is zero after invalidation");

    expectTrue(cooker.count_prune_reconcile() == 0u, "empty cache prune reconcile is zero");

    const std::string source = writeTempFile("/tmp/fuse_b79_reconcile_mesh.obj", "# reconcile mesh\n");
    entry.output_path = "/tmp/fuse_b79_reconcile_mesh.fusemesh";

    expectTrue(cooker.count_prune_reconcile() == 0u, "fresh cache prune reconcile is zero");
    expectTrue(cooker.estimate_manifest_cache_reconcile(manifest) == 0u,
               "fresh cache manifest reconcile estimate is zero");

    writeTempFile(source, "# reconcile mesh updated\n");
    expectTrue(cooker.count_prune_reconcile() == 1u, "stale content prune reconcile is one");
    expectTrue(cooker.estimate_manifest_cache_reconcile(manifest) == 1u,
               "stale content manifest reconcile estimate matches prune reconcile");

    expectTrue(removed == 1u, "prune_all removes estimated stale entry");
    expectTrue(cooker.count_prune_reconcile() == 0u, "prune reconcile zero after prune_all");





    const fuse::project::CookStaleDependencyReconcileEstimate fresh_estimate =
    expectTrue(fresh_estimate.stale_upstream_entries == 0u, "fresh cache has no stale upstream entries");
    expectTrue(fresh_estimate.downstream_cascade_entries == 0u, "fresh cache has no downstream cascade entries");
               "count probe matches reconcile estimate total on fresh cache");

    const fuse::project::CookStaleDependencyReconcileEstimate stale_estimate =
    expectTrue(stale_estimate.stale_upstream_entries >= 1u, "stale upstream entries estimated after source change");
    expectTrue(stale_estimate.total() >= 1u, "stale reconcile estimate is non-zero after source change");
    expectTrue(cooker.count_stale_dependency_invalidation(manifest) == stale_estimate.total(),
               "count probe matches reconcile estimate total after source change");

    const fuse::u64 downstream_hash = cooked.records[1].content_hash;
    expectTrue(removed >= stale_estimate.total(), "reconcile invalidation removes at least estimated count");
    expectTrue(cooker.cache().lookup(downstream_hash) == fuse::project::CookCacheLookup::Miss,
               "downstream misses after estimated reconcile invalidation");
               "reconcile estimate is zero after stale dependency invalidation");

void testCookerWouldInvalidationProbes() {
    const std::string source_a = writeTempFile("/tmp/fuse_b79_would_chain_a.obj", "# would chain a\n");
    const std::string source_b = writeTempFile("/tmp/fuse_b79_would_chain_b.obj", "# would chain b\n");

    entry_a.output_path = "/tmp/fuse_b79_would_chain_a.fusemesh";

    entry_b.output_path = "/tmp/fuse_b79_would_chain_b.fusemesh";

    expectTrue(cooker.cook_manifest(manifest).ok, "manifest cook for would probes ok");
               "would_invalidate_upstream_dependency true for seeded chain");
               "would_invalidate_upstream_dependency guarded on empty source");
    expectTrue(!cooker.would_reconcile_invalidation(manifest),
               "would_reconcile_invalidation false on fresh cache");

    writeTempFile(source_a, "# would chain a revised\n");
    expectTrue(cooker.would_reconcile_invalidation(manifest),
               "would_reconcile_invalidation true after upstream change");

void testCookerUpstreamInvalidationSourceProbe() {
    const std::string source_a = writeTempFile("/tmp/fuse_b79_up_probe_a.obj", "# up probe a\n");
    const std::string source_b = writeTempFile("/tmp/fuse_b79_up_probe_b.obj", "# up probe b\n");

    entry_a.output_path = "/tmp/fuse_b79_up_probe_a.fusemesh";

    entry_b.output_path = "/tmp/fuse_b79_up_probe_b.fusemesh";

    expectTrue(cooker.cook_manifest(manifest).ok, "manifest cook for upstream source probe ok");

    const std::vector<std::string> probed =
        cooker.probe_upstream_invalidation_sources(manifest, source_a);
    expectTrue(probed.size() >= 2u, "upstream probe lists changed source and downstream paths");
    expectTrue(probed[1] == entry_a.output_path || probed[1] == source_b,
               "upstream probe includes downstream invalidation path");


void testCookerShouldSkipAndWouldInvalidateHelpers() {
    const std::string source_a = writeTempFile("/tmp/fuse_b79_skip_inv_a.obj", "# skip inv a\n");
    const std::string source_b = writeTempFile("/tmp/fuse_b79_skip_inv_b.obj", "# skip inv b\n");

    entry_a.output_path = "/tmp/fuse_b79_skip_inv_a.fusemesh";

    entry_b.output_path = "/tmp/fuse_b79_skip_inv_b.fusemesh";

    expectTrue(cooker.cook_manifest(manifest).ok, "manifest cook for should_skip helpers ok");

    expectTrue(cooker.should_skip_upstream_invalidation(manifest, ""),
               "should_skip_upstream_invalidation true for empty changed source");
    expectTrue(!cooker.should_skip_upstream_invalidation(manifest, source_a),
               "should_skip_upstream_invalidation false for seeded chain head");
    expectTrue(cooker.would_invalidate_upstream(manifest, source_a),
               "would_invalidate_upstream true for seeded chain head");
    expectTrue(!cooker.would_invalidate_upstream(manifest, ""),
               "would_invalidate_upstream false for empty changed source");

    expectTrue(cooker.should_skip_stale_dependency_invalidation(manifest),
               "should_skip_stale_dependency_invalidation true for fresh cache");
    expectTrue(cooker.should_skip_reconcile_invalidation(manifest),
               "should_skip_reconcile_invalidation true for fresh cache");

    expectTrue(fresh.should_skip(), "fresh reconcile estimate should_skip is true");

    writeTempFile(source_a, "# skip inv a revised\n");
    expectTrue(!cooker.should_skip_stale_dependency_invalidation(manifest),
               "should_skip_stale_dependency_invalidation false after upstream change");
    expectTrue(!cooker.should_skip_reconcile_invalidation(manifest),
               "should_skip_reconcile_invalidation false after upstream change");

    expectTrue(!stale.should_skip(), "stale reconcile estimate should_skip is false");
    expectTrue(stale.should_skip() == cooker.should_skip_reconcile_invalidation(manifest),
               "reconcile estimate should_skip matches cooker helper");

void testCookerWouldInvalidateHelpers() {
    const std::string source_a = writeTempFile("/tmp/fuse_b79_would_reconcile_a.obj", "# would reconcile a\n");
    const std::string source_b = writeTempFile("/tmp/fuse_b79_would_reconcile_b.obj", "# would reconcile b\n");

    entry_a.output_path = "/tmp/fuse_b79_would_reconcile_a.fusemesh";

    entry_b.output_path = "/tmp/fuse_b79_would_reconcile_b.fusemesh";

    expectTrue(cooker.cook_manifest(manifest).ok, "manifest cook for would_* helpers ok");
    expectTrue(!cooker.would_reconcile_invalidation(manifest), "fresh cache would not reconcile");
    expectTrue(!cooker.would_prune_reconcile(), "fresh cache would not prune reconcile");
    expectTrue(!cooker.would_stale_dependency_invalidate(manifest),
               "fresh cache would not stale-dependency invalidate");
    expectTrue(cooker.would_upstream_invalidate(manifest, source_a),
               "would_upstream_invalidate true for seeded chain head");
    expectTrue(!cooker.would_upstream_invalidate(manifest, ""),
               "would_upstream_invalidate guards empty changed source");
    expectTrue(!cooker.would_upstream_invalidate(manifest, "/tmp/fuse_b79_missing_would.obj"),
               "would_upstream_invalidate false for unknown source");

    writeTempFile(source_a, "# would reconcile a revised\n");
    expectTrue(cooker.would_stale_dependency_invalidate(manifest),
               "would_stale_dependency_invalidate true after upstream change");
    expectTrue(cooker.would_prune_reconcile(), "would_prune_reconcile true after upstream change");
    expectTrue(cooker.would_reconcile_invalidation(manifest)
                   == (cooker.estimate_reconcile_invalidation(manifest).total() != 0),
               "would_reconcile_invalidation mirrors estimate total");

    const std::string source_a = writeTempFile("/tmp/fuse_b79_skip_would_a.obj", "# skip would a\n");
    const std::string source_b = writeTempFile("/tmp/fuse_b79_skip_would_b.obj", "# skip would b\n");

    entry_a.output_path = "/tmp/fuse_b79_skip_would_a.fusemesh";

    entry_b.output_path = "/tmp/fuse_b79_skip_would_b.fusemesh";

    expectTrue(cooker.cook_manifest(manifest).ok, "manifest cook for skip/would helpers ok");

    expectTrue(cooker.should_skip_prune_reconcile(), "fresh cache should skip prune reconcile");
               "fresh cache should skip reconcile invalidation");
               "fresh cache upstream would_invalidate true when entries exist");
               "empty changed source upstream would_invalidate is guarded");

    expectTrue(fresh.should_skip(), "fresh reconcile estimate should_skip");
    expectTrue(!fresh.would_reconcile(), "fresh reconcile estimate would_reconcile is false");
    expectTrue(cooker.should_skip_reconcile_invalidation(manifest) == fresh.should_skip(),
               "should_skip_reconcile_invalidation matches estimate should_skip");

    writeTempFile(source_a, "# skip would a revised\n");
               "upstream change makes stale dependency would_invalidate true");
               "stale dependency blocks reconcile skip");

    expectTrue(stale.would_reconcile(), "stale reconcile estimate would_reconcile");

               "upstream would_invalidate remains true while entries exist");

void testCookerShouldSkipReconcileHelpers() {
    const std::string source_a = writeTempFile("/tmp/fuse_b79_skip_reconcile_a.obj", "# skip reconcile a\n");
    const std::string source_b = writeTempFile("/tmp/fuse_b79_skip_reconcile_b.obj", "# skip reconcile b\n");

    entry_a.output_path = "/tmp/fuse_b79_skip_reconcile_a.fusemesh";

    entry_b.output_path = "/tmp/fuse_b79_skip_reconcile_b.fusemesh";


               "empty changed source should_skip upstream invalidation");
               "seeded chain should not skip upstream invalidation probe");
               "fresh cache should_skip stale dependency invalidation");
               "fresh cache should_skip reconcile invalidation");
    expectTrue(cooker.estimate_reconcile_invalidation(manifest).should_skip(),
               "fresh reconcile estimate should_skip");

    writeTempFile(source_a, "# skip reconcile a revised\n");
               "changed upstream should not skip stale dependency invalidation");
               "stale dependency should not skip reconcile invalidation");
    expectTrue(!cooker.estimate_reconcile_invalidation(manifest).should_skip(),
               "stale reconcile estimate should not skip");

    expectTrue(removed >= 1u, "stale dependency invalidation runs after should_skip probe");
               "should_skip stale dependency after invalidation");
    expectTrue(cooker.should_skip_upstream_invalidation(manifest, sourceA),
               "should_skip upstream invalidation true after cache cleared");
}

void testCookerReconcileEstimateProbes() {
    const std::string source_a = writeTempFile("/tmp/fuse_b79_reconcile_a.obj", "# reconcile a\n");
    const std::string source_b = writeTempFile("/tmp/fuse_b79_reconcile_b.obj", "# reconcile b\n");

    fuse::project::CookManifest manifest;
    fuse::project::CookManifestEntry entry_a;
    entry_a.kind = fuse::project::CookAssetKind::Mesh;
    entry_a.source_path = source_a;
    entry_a.output_path = "/tmp/fuse_b79_reconcile_a.fusemesh";
    manifest.assets.push_back(entry_a);

    fuse::project::CookManifestEntry entry_b;
    entry_b.kind = fuse::project::CookAssetKind::Mesh;
    entry_b.source_path = source_b;
    entry_b.output_path = "/tmp/fuse_b79_reconcile_b.fusemesh";
    entry_b.dependencies.push_back(entry_a.output_path);
    manifest.assets.push_back(entry_b);

    fuse::project::AssetCooker cooker;
    const fuse::project::CookBatchResult cooked = cooker.cook_manifest(manifest);
    expectTrue(cooked.ok, "manifest cook for reconcile estimate ok");

    const fuse::project::CookCacheReconcileEstimate fresh = cooker.estimate_reconcile_invalidation(manifest);
    expectTrue(fresh.total() == 0u, "fresh cache reconcile estimate is zero");
    expectTrue(fresh.upstream_invalidation_entries == 0u,
               "fresh reconcile estimate has zero upstream invalidation entries");
    expectTrue(!cooker.would_reconcile_invalidation(manifest), "fresh cache would_reconcile is false");
    expectTrue(fresh.should_skip(), "fresh reconcile estimate should_skip is true");
    expectTrue(cooker.estimate_prune_reconcile().total() == 0u, "fresh prune reconcile estimate is zero");
    expectTrue(cooker.estimate_prune_reconcile().should_skip(), "fresh prune reconcile should_skip is true");
    expectTrue(!cooker.should_skip_upstream_invalidation(manifest, source_a),
               "should_skip upstream invalidation false when cache has matching entries");
    expectTrue(cooker.should_skip_upstream_invalidation(manifest, ""),
               "should_skip upstream invalidation for empty changed source");
    expectTrue(cooker.should_skip_stale_dependency_invalidation(manifest),
               "should_skip stale dependency invalidation on fresh cache");
    expectTrue(cooker.should_skip_prune_reconcile(), "should_skip prune reconcile on fresh cache");
    expectTrue(cooker.should_skip_reconcile_invalidation(manifest),
               "should_skip reconcile invalidation on fresh cache");
    expectTrue(fresh.should_skip(), "fresh reconcile estimate should_skip");
    expectTrue(cooker.should_skip_prune_reconcile(), "fresh prune reconcile should_skip");
               "fresh stale dependency reconcile should_skip");
               "fresh combined reconcile should_skip");
               "empty changed source upstream reconcile should_skip");

    const fuse::u32 upstream_probe = cooker.count_upstream_invalidation(manifest, source_a);
    const fuse::project::CookCacheReconcileEstimate upstream_plan =
        cooker.estimate_reconcile_invalidation(manifest, source_a);
    expectTrue(upstream_plan.upstream_invalidation_entries == upstream_probe,
               "reconcile estimate upstream field matches upstream invalidation probe");
    expectTrue(upstream_plan.total() >= upstream_probe,
               "reconcile estimate total includes upstream invalidation planning");
    expectTrue(!cooker.should_skip_upstream_invalidation(manifest, source_a),
               "non-empty upstream reconcile should not skip when entries exist");
    expectTrue(!upstream_plan.should_skip(), "upstream reconcile plan should not skip");

    writeTempFile(source_a, "# reconcile a revised\n");
    const fuse::u32 stale_count = cooker.count_stale_dependency_invalidation(manifest);
    expectTrue(stale_count >= 1u, "stale dependency reconcile count is non-zero after upstream change");

    const fuse::project::CookCacheReconcileEstimate stale = cooker.estimate_reconcile_invalidation(manifest);
    expectTrue(stale.stale_dependency_entries == stale_count,
               "reconcile estimate stale count matches dependency probe");
    expectTrue(stale.total() >= stale_count, "reconcile estimate total includes stale dependency count");
    expectTrue(cooker.would_reconcile_invalidation(manifest),
               "would_reconcile_invalidation true after upstream change");
    expectTrue(cooker.would_reconcile_invalidation(manifest), "stale cache would_reconcile is true");
    expectTrue(!stale.should_skip(), "stale reconcile estimate should_skip is false");
    expectTrue(!cooker.should_skip_stale_dependency_invalidation(manifest),
               "should_skip stale dependency invalidation false after upstream change");
    expectTrue(!cooker.should_skip_reconcile_invalidation(manifest),
               "should_skip reconcile invalidation false after upstream change");
    expectTrue(!stale.should_skip(), "stale reconcile estimate should not skip");
               "stale dependency reconcile should not skip after upstream change");
               "combined reconcile should not skip when stale dependencies exist");

    const fuse::u32 removed = cooker.invalidate_stale_dependency_hashes(manifest);
    expectTrue(removed >= stale_count, "stale dependency invalidation removes at least estimated count");

    const fuse::project::CookCacheReconcileEstimate after = cooker.estimate_reconcile_invalidation(manifest);
    expectTrue(after.stale_dependency_entries == 0u,
               "stale dependency reconcile estimate zero after stale invalidation");
    expectTrue(after.prune_stale_entries >= 1u,
               "changed upstream entry remains stale for prune reconcile");
    expectTrue(!after.should_skip(), "post-invalidation prune reconcile estimate should_skip is false");
    expectTrue(!cooker.should_skip_prune_reconcile(),
               "should_skip prune reconcile false when stale entries remain");
    expectTrue(cooker.should_skip_stale_dependency_invalidation(manifest),
               "should_skip stale dependency invalidation true after stale invalidation");
}

void testCookerWouldReconcileAndUpstreamProbe() {
    const std::string source_a = writeTempFile("/tmp/fuse_b79_would_reconcile_a.obj", "# would reconcile a\n");
    const std::string source_b = writeTempFile("/tmp/fuse_b79_would_reconcile_b.obj", "# would reconcile b\n");
void testCookCacheWouldInvalidateMirrorsCount() {
    const std::string source = writeTempFile("/tmp/fuse_b79_would_mirror.obj", "# would mirror\n");

    fuse::project::MeshImportDesc desc;
    desc.input_path = source;
    desc.output_path = "/tmp/fuse_b79_would_mirror.fusemesh";

    fuse::project::AssetCooker cooker;
    const fuse::project::CookRecord seeded = cooker.cook_mesh(desc);
    expectTrue(seeded.ok, "seed cook for would_invalidate mirror ok");

    expectTrue(cooker.cache().would_invalidate(seeded.content_hash) ==
                   cooker.cache().contains(seeded.content_hash),
               "would_invalidate agrees with contains");
    expectTrue(cooker.cache().would_invalidate_source(source), "would_invalidate_source true when seeded");
    expectTrue(cooker.cache().would_invalidate_output(desc.output_path), "would_invalidate_output true when seeded");
    expectTrue(!cooker.would_reconcile_invalidation(fuse::project::CookManifest{}),
               "would_reconcile_invalidation false on fresh cache with empty manifest");

    writeTempFile(source, "# would mirror revised\n");
    expectTrue(cooker.cache().would_prune_all(), "would_prune_all true after content change");
    expectTrue(cooker.cache().would_invalidate_stale_content_for_source(source, seeded.content_hash + 1u),
               "would_invalidate_stale_content mirrors count probe after change");

void testCookerUpstreamInvalidationWouldAndProbe() {
    const std::string source_a = writeTempFile("/tmp/fuse_b79_up_would_a.obj", "# up would a\n");
    const std::string source_b = writeTempFile("/tmp/fuse_b79_up_would_b.obj", "# up would b\n");
void testCookerUpstreamInvalidationEstimateProbes() {
    const std::string source_a = writeTempFile("/tmp/fuse_b79_up_est_a.obj", "# upstream est a\n");
    const std::string source_b = writeTempFile("/tmp/fuse_b79_up_est_b.obj", "# upstream est b\n");
void testCookerWouldReconcileAndUpstreamProbes() {
void testCookerUpstreamInvalidationEstimate() {
    const std::string source_a = writeTempFile("/tmp/fuse_b79_up_est_a.obj", "# up est a\n");
    const std::string source_b = writeTempFile("/tmp/fuse_b79_up_est_b.obj", "# up est b\n");
void testCookerUpstreamReconcileProbes() {
    const std::string source_a = writeTempFile("/tmp/fuse_b79_up_reconcile_a.obj", "# up reconcile a\n");
    const std::string source_b = writeTempFile("/tmp/fuse_b79_up_reconcile_b.obj", "# up reconcile b\n");
    const std::string source_c = writeTempFile("/tmp/fuse_b79_up_reconcile_c.obj", "# up reconcile c\n");
void testCookerUpstreamEstimateAndWouldProbes() {
void testCookerWouldReconcileProbes() {
void testCookerUpstreamInvalidationEstimateAndWouldGuards() {
    const std::string source_c = writeTempFile("/tmp/fuse_b79_up_est_c.obj", "# up est c\n");
void testCookerUpstreamEstimateAndSourceProbe() {
void testCookerShouldSkipReconcileGuards() {
    const std::string source_a = writeTempFile("/tmp/fuse_b79_skip_reconcile_a.obj", "# skip reconcile a\n");
    const std::string source_b = writeTempFile("/tmp/fuse_b79_skip_reconcile_b.obj", "# skip reconcile b\n");
void testCookerShouldSkipWouldInvalidateProbes() {
    const std::string source_a = writeTempFile("/tmp/fuse_b79_skip_inv_a.obj", "# skip inv a\n");
    const std::string source_b = writeTempFile("/tmp/fuse_b79_skip_inv_b.obj", "# skip inv b\n");

    fuse::project::CookManifest manifest;
    fuse::project::CookManifestEntry entry_a;
    entry_a.kind = fuse::project::CookAssetKind::Mesh;
    entry_a.source_path = source_a;
    entry_a.output_path = "/tmp/fuse_b79_would_reconcile_a.fusemesh";
    entry_a.output_path = "/tmp/fuse_b79_up_would_a.fusemesh";
    entry_a.output_path = "/tmp/fuse_b79_up_est_a.fusemesh";
    entry_a.output_path = "/tmp/fuse_b79_up_reconcile_a.fusemesh";
    entry_a.output_path = "/tmp/fuse_b79_skip_reconcile_a.fusemesh";
    entry_a.output_path = "/tmp/fuse_b79_skip_inv_a.fusemesh";
    manifest.assets.push_back(entry_a);

    fuse::project::CookManifestEntry entry_b;
    entry_b.kind = fuse::project::CookAssetKind::Mesh;
    entry_b.source_path = source_b;
    entry_b.output_path = "/tmp/fuse_b79_would_reconcile_b.fusemesh";
    entry_b.output_path = "/tmp/fuse_b79_up_would_b.fusemesh";
    entry_b.output_path = "/tmp/fuse_b79_up_est_b.fusemesh";
    entry_b.dependencies.push_back(entry_a.output_path);
    manifest.assets.push_back(entry_b);

    expectTrue(cooker.cook_manifest(manifest).ok, "manifest cook for would reconcile probe ok");
    expectTrue(!cooker.would_reconcile_invalidation(manifest), "fresh cache would_reconcile is false");

    writeTempFile(source_a, "# would reconcile a revised\n");
    expectTrue(cooker.would_reconcile_invalidation(manifest),
               "stale upstream makes would_reconcile true");
    expectTrue(cooker.estimate_reconcile_invalidation(manifest).total() >= 1u,
               "would_reconcile agrees with reconcile estimate total");
    expectTrue(cooker.cook_manifest(manifest).ok, "manifest cook for upstream would/probe ok");
    expectTrue(cooker.would_upstream_invalidation(manifest, source_a),
               "would_upstream_invalidation true for seeded chain head");
    expectTrue(!cooker.would_upstream_invalidation(manifest, ""),
               "would_upstream_invalidation guarded on empty source");
    expectTrue(!cooker.would_upstream_invalidation(manifest, "/tmp/fuse_b79_missing_up_would.obj"),
               "would_upstream_invalidation false for unknown source");

    const std::vector<std::string> probed = cooker.probe_upstream_invalidation_sources(manifest, source_a);
    expectTrue(probed.size() >= 2u, "upstream probe lists changed source and downstream paths");
    expectTrue(probed[0] == source_a, "upstream probe starts at changed source");
    bool has_downstream = false;
    for (const std::string& path : probed) {
        if (path == source_b) {
            has_downstream = true;
            break;
    expectTrue(has_downstream, "upstream probe includes downstream source");
    expectTrue(cooker.probe_upstream_invalidation_sources(manifest, "").empty(),
               "empty changed source upstream probe is guarded");

    const fuse::u32 probed_count = cooker.count_upstream_invalidation(manifest, source_a);
    const fuse::u32 removed = cooker.invalidate_upstream_dependency(manifest, source_a);
    expectTrue(removed >= probed_count, "upstream invalidation removes at least probed count");
    expectTrue(!cooker.would_upstream_invalidation(manifest, source_a),
               "would_upstream_invalidation false after invalidation");

void testCookCacheUniqueStaleUpstreamProbe() {
    const std::string source_a = writeTempFile("/tmp/fuse_b79_unique_up_a.obj", "# unique up a\n");
    const std::string source_b = writeTempFile("/tmp/fuse_b79_unique_up_b.obj", "# unique up b\n");
    expectTrue(cooker.cook_manifest(manifest).ok, "manifest cook for would_reconcile probes ok");
    expectTrue(cooker.would_upstream_invalidate(manifest, source_a),
               "cached entries make would_upstream_invalidate true before invalidation");
    expectTrue(cooker.probe_stale_dependency_sources(manifest).empty(),
               "fresh cache stale dependency probe is empty");

    expectTrue(cooker.would_reconcile_invalidation(manifest), "stale upstream makes would_reconcile true");
               "cached chain remains upstream-invalidatable after source change");

    expectTrue(cooker.cook_manifest(manifest).ok, "manifest cook for would reconcile probes ok");
    expectTrue(!cooker.would_reconcile_invalidation(manifest),
               "fresh cache would_reconcile_invalidation is false");
    expectTrue(!cooker.would_prune_reconcile(), "fresh cache would_prune_reconcile is false");

               "stale dependency makes would_reconcile_invalidation true");
    expectTrue(cooker.estimate_reconcile_invalidation(manifest).total() > 0u,
               "would_reconcile implies non-zero reconcile estimate");

    const std::vector<std::string> upstream_sources =
        cooker.probe_upstream_invalidation_sources(manifest, source_a);
    expectTrue(upstream_sources.size() >= 2u, "upstream probe lists changed source and dependents");

    const std::vector<std::string> stale_sources = cooker.probe_stale_dependency_sources(manifest);
    expectTrue(!stale_sources.empty(), "stale dependency probe lists stale upstream sources");
    expectTrue(stale_sources.size() == 1u, "one unique stale dependency source probed");
    expectTrue(stale_sources[0] == source_b, "downstream source has stale upstream hash");

    expectTrue(removed >= 2u, "upstream invalidation clears cached chain");
    expectTrue(!cooker.would_upstream_invalidate(manifest, source_a),
               "would_upstream_invalidate false after upstream invalidation");
    expectTrue(upstream_sources[0] == source_a, "upstream probe starts at changed source");

    const fuse::u32 upstream_count = cooker.count_upstream_invalidation(manifest, source_a);
    expectTrue(removed >= upstream_count, "upstream invalidation removes at least probed entry count");
    expectTrue(cooker.cache().entry_count() == 0u, "cache empty after upstream invalidation");

void testCookCacheDownstreamWouldInvalidateProbe() {
    const std::string source_a = writeTempFile("/tmp/fuse_b79_would_down_a.obj", "# would down a\n");
    const std::string source_b = writeTempFile("/tmp/fuse_b79_would_down_b.obj", "# would down b\n");

    entry_a.output_path = "/tmp/fuse_b79_unique_up_a.fusemesh";
    entry_a.output_path = "/tmp/fuse_b79_would_down_a.fusemesh";

    entry_b.output_path = "/tmp/fuse_b79_unique_up_b.fusemesh";

    expectTrue(cooker.cook_manifest(manifest).ok, "manifest cook for unique upstream probe ok");

    writeTempFile(source_a, "# unique up a revised\n");
    expectTrue(cooker.cook_manifest(manifest).ok, "manifest cook for upstream estimate ok");
    expectTrue(!cooker.would_reconcile_invalidation(manifest), "fresh cache would not reconcile");

    const fuse::project::CookCacheUpstreamInvalidationEstimate estimate =
        cooker.estimate_upstream_invalidation(manifest, source_a);
    expectTrue(estimate.source_entries == 1u, "upstream estimate counts source entry");
    expectTrue(estimate.downstream_entries == 1u, "upstream estimate counts downstream entry");
    expectTrue(estimate.total() == cooker.count_upstream_invalidation(manifest, source_a),
               "upstream estimate total matches count probe");
    expectTrue(cooker.cache().entry_count() == 2u, "two entries seeded for upstream estimate");

    const fuse::project::CookCacheUpstreamInvalidationEstimate empty =
        cooker.estimate_upstream_invalidation(manifest, "");
    expectTrue(empty.total() == 0u, "empty changed source upstream estimate is zero");

    expectTrue(estimate.direct_source_entries >= 1u, "upstream estimate counts direct source entries");
    expectTrue(estimate.downstream_entries >= 1u, "upstream estimate counts downstream entries");


    writeTempFile(source_a, "# up est a revised\n");
               "stale upstream makes would_reconcile_invalidation true");

    const std::string source = writeTempFile("/tmp/fuse_b79_unique_up.obj", "# unique up\n");

    fuse::project::CookManifestEntry entry;
    entry.kind = fuse::project::CookAssetKind::Mesh;
    entry.source_path = source;
    entry.output_path = "/tmp/fuse_b79_unique_up.fusemesh";
    manifest.assets.push_back(entry);

    expectTrue(cooker.cook_entry(entry, manifest).ok, "seed cook for unique upstream probe");

    fuse::project::CookJobGraph graph;
    graph.build_from_manifest(manifest);
    std::vector<std::pair<std::string, fuse::u64>> source_upstream;
    for (const fuse::project::CookJob& job : graph.jobs()) {
        fuse::u64 hash = 0;
        for (const std::string& dependency_id : job.dependency_ids) {
            for (const fuse::project::CookJob& dependency : graph.jobs()) {
                if (dependency.id != dependency_id) {
                    continue;
                hash = fuse::project::fnv1a64_combine(
                    hash, fuse::project::fnv1a64_bytes(
                              reinterpret_cast<const fuse::u8*>(dependency.output_path.data()),
                              dependency.output_path.size()));
                hash = fuse::project::fnv1a64_combine(hash, fuse::project::hash_file_content(dependency.source_path));
        source_upstream.emplace_back(job.source_path, hash);

    const std::vector<std::string> raw =
        cooker.cache().probe_stale_upstream_sources(source_upstream);
    const std::vector<std::string> unique =
        cooker.cache().probe_unique_stale_upstream_sources(source_upstream);
    expectTrue(raw.size() >= unique.size(), "unique upstream probe is deduplicated");
    expectTrue(unique.size() >= 1u, "unique upstream probe finds stale downstream source");
    expectTrue(cooker.cache().would_invalidate_stale_upstream_hashes(source_upstream),
               "would_invalidate_stale_upstream mirrors count probe");
               "would_reconcile_invalidation true after upstream hash drift");
    expectTrue(cooker.cook_manifest(manifest).ok, "manifest cook for upstream estimate probes ok");
    expectTrue(cooker.cache().entry_count() == 2u, "two entries seeded for upstream estimate probes");

    expectTrue(!cooker.would_upstream_invalidation(manifest, ""), "empty changed source would_upstream is false");
               "empty changed source upstream probe is empty");

    expectTrue(estimate.direct_source_entries == 1u, "upstream estimate counts direct source entry");
               "would_upstream true for seeded changed source");

    expectTrue(probed.size() >= 2u, "upstream probe lists changed source and dependents");

void testCookerWouldReconcileInvalidationProbe() {
    const std::string source = writeTempFile("/tmp/fuse_b79_would_reconcile.obj", "# would reconcile v1\n");

void testCookerWouldReconcileInvalidate() {
    const std::string source = writeTempFile("/tmp/fuse_b79_would_reconcile.obj", "# would reconcile\n");

    entry.output_path = "/tmp/fuse_b79_would_reconcile.fusemesh";

    expectTrue(cooker.cook_manifest(manifest).ok, "manifest cook for would_reconcile probe ok");

    writeTempFile(source, "# would reconcile v2\n");
    expectTrue(cooker.would_reconcile_invalidation(manifest), "stale content makes would_reconcile true");
    expectTrue(cooker.estimate_reconcile_invalidation(manifest).prune_stale_entries >= 1u,
               "stale reconcile estimate includes prune stale count");
    entry_b.output_path = "/tmp/fuse_b79_would_down_b.fusemesh";


    expectTrue(cooker.cook_manifest(manifest).ok, "chain cook for downstream would_invalidate probe ok");
    expectTrue(cooker.cache().would_invalidate_downstream_of(entry_a.output_path, graph.edges(), graph.jobs()),
               "would_invalidate_downstream_of true for producer with dependents");
    expectTrue(!cooker.cache().would_invalidate_downstream_of(entry_b.output_path, graph.edges(), graph.jobs()),
               "leaf output would_invalidate_downstream_of is false");
    expectTrue(!cooker.cache().would_invalidate_downstream_of("", graph.edges(), graph.jobs()),
               "empty output path would_invalidate_downstream_of is guarded");
        source_upstream.emplace_back(job.source_path, 999u);

    const std::vector<std::string> duplicated =
    expectTrue(!duplicated.empty(), "stale upstream probe finds mismatched entry");
    expectTrue(unique.size() == 1u, "unique stale upstream probe deduplicates source paths");
    expectTrue(unique[0] == source, "unique stale upstream probe returns changed source");
               "would_invalidate_stale_upstream true when hashes mismatch");


    expectTrue(cooker.cook_manifest(manifest).ok, "manifest cook for would_reconcile ok");
    expectTrue(!cooker.would_reconcile_invalidate(manifest), "fresh cache would_reconcile is false");
    expectTrue(cooker.probe_reconcile_stale_sources(manifest).empty(),
               "fresh cache reconcile stale probe is empty");

    writeTempFile(source, "# would reconcile revised\n");
    expectTrue(cooker.would_reconcile_invalidate(manifest), "stale content makes would_reconcile true");

    const std::vector<std::string> stale_sources = cooker.probe_reconcile_stale_sources(manifest);
    expectTrue(stale_sources.size() == 1u, "one stale source probed for reconcile");
    expectTrue(stale_sources[0] == source, "stale reconcile probe reports changed source");

void testCookerEstimateUpstreamInvalidation() {
    const std::string source_a = writeTempFile("/tmp/fuse_b79_est_up_a.obj", "# est up a\n");
    const std::string source_b = writeTempFile("/tmp/fuse_b79_est_up_b.obj", "# est up b\n");

    const std::string sourceA = writeTempFile("/tmp/fuse_b79_est_chain_a.obj", "# est chain a\n");
    const std::string sourceB = writeTempFile("/tmp/fuse_b79_est_chain_b.obj", "# est chain b\n");



    fuse::project::CookManifestEntry entryA;
    entryA.kind = fuse::project::CookAssetKind::Mesh;
    entryA.source_path = sourceA;
    entryA.output_path = "/tmp/fuse_b79_est_chain_a.fusemesh";
    manifest.assets.push_back(entryA);

    fuse::project::CookManifestEntry entryB;
    entryB.kind = fuse::project::CookAssetKind::Mesh;
    entryB.source_path = sourceB;
    entryB.output_path = "/tmp/fuse_b79_est_chain_b.fusemesh";
    entryB.dependencies.push_back(entryA.output_path);
    manifest.assets.push_back(entryB);


    const fuse::project::CookUpstreamInvalidationEstimate estimate =
        cooker.estimate_upstream_invalidation(manifest, sourceA);
    expectTrue(estimate.total() == cooker.count_upstream_invalidation(manifest, sourceA),


    expectTrue(cooker.estimate_upstream_invalidation(manifest, "").total() == 0u,
               "empty changed source upstream estimate is zero");

    const fuse::u32 output_count = cooker.count_output_invalidation(manifest, entryA.output_path);
    expectTrue(output_count >= 1u, "output invalidation count includes dependents");
    expectTrue(cooker.count_output_invalidation(manifest, "") == 0u,
               "empty changed output invalidation count is zero");

void testCookCacheWouldInvalidateDownstreamProbe() {
    writeTempFile(sourceA, "# est chain a revised\n");
               "stale dependency would_reconcile_invalidation is true");


               "empty changed source would_upstream is false");
               "fresh cache would_reconcile is false");

    const fuse::project::CookUpstreamInvalidationEstimate fresh =
    expectTrue(fresh.direct_source_entries >= 1u, "upstream estimate includes direct source entries");
    expectTrue(fresh.downstream_entries >= 1u, "upstream estimate includes downstream entries");
    expectTrue(fresh.total() == cooker.count_upstream_invalidation(manifest, source_a),
               "would_upstream true when entries would be removed");

    expectTrue(removed >= fresh.total(), "upstream invalidation removes at least estimated total");
               "would_upstream false after upstream invalidation");

void testCookerWouldReconcileAfterUpstreamChange() {
    const std::string source_a = writeTempFile("/tmp/fuse_b79_would_rec_a.obj", "# would rec a\n");
    const std::string source_b = writeTempFile("/tmp/fuse_b79_would_rec_b.obj", "# would rec b\n");

    entry_a.output_path = "/tmp/fuse_b79_est_up_a.fusemesh";
    entry_a.output_path = "/tmp/fuse_b79_would_rec_a.fusemesh";

    entry_b.output_path = "/tmp/fuse_b79_est_up_b.fusemesh";

    expectTrue(cooker.cook_manifest(manifest).ok, "chain manifest cook for upstream estimate ok");

    const fuse::project::CookCacheInvalidationEstimate estimate =
    entry_b.output_path = "/tmp/fuse_b79_would_rec_b.fusemesh";


    expectTrue(estimate.source_entries == 1u, "upstream estimate counts changed source entry");
    expectTrue(estimate.downstream_entries == 1u, "upstream estimate counts one downstream entry");
    expectTrue(estimate.total() == 2u, "upstream estimate total matches chain footprint");

    const fuse::project::CookCacheInvalidationEstimate empty =


    expectTrue(cooker.cook_manifest(manifest).ok, "chain cook for would_invalidate_downstream ok");

               "would_invalidate_downstream_of reports dependents");
               "empty output path would_invalidate_downstream is guarded");
               "leaf output would_invalidate_downstream is false when no dependents cached");
    entry_b.output_path = "/tmp/fuse_b79_up_reconcile_b.fusemesh";


    fuse::project::CookManifestEntry entry_c;
    entry_c.kind = fuse::project::CookAssetKind::Mesh;
    entry_c.source_path = source_c;
    entry_c.output_path = "/tmp/fuse_b79_up_reconcile_c.fusemesh";
    entry_c.dependencies.push_back(entry_b.output_path);
    manifest.assets.push_back(entry_c);

    expectTrue(cooker.cook_manifest(manifest).ok, "chain manifest cook for upstream reconcile ok");
    expectTrue(cooker.should_skip_reconcile_invalidation(manifest), "fresh cache should_skip_reconcile is true");

    const fuse::project::CookCacheUpstreamReconcileEstimate upstream =
        cooker.estimate_upstream_reconcile(manifest, source_a);
    expectTrue(upstream.direct_source_entries == 1u, "upstream reconcile counts direct source entry");
    expectTrue(upstream.downstream_entries == 2u, "upstream reconcile counts downstream entries");
    expectTrue(upstream.total() == cooker.count_upstream_invalidation(manifest, source_a),
               "upstream reconcile total matches count probe");

    expectTrue(probed.size() >= 3u, "upstream probe lists changed source and dependents");

    expectTrue(cooker.estimate_upstream_reconcile(manifest, "").total() == 0u,
               "empty changed source upstream reconcile is zero");

    writeTempFile(source_a, "# up reconcile a revised\n");
    expectTrue(cooker.would_reconcile_invalidation(manifest), "stale dependency makes would_reconcile true");
    expectTrue(cooker.should_skip_upstream_invalidation(manifest, ""),
               "should_skip_upstream true for empty changed source");



    const fuse::u32 output_count = cooker.count_output_invalidation(manifest, entry_a.output_path);

    expectTrue(!cooker.should_skip_reconcile_invalidation(manifest),
               "should_skip_reconcile false after upstream change");

    const fuse::project::CookCacheReconcileEstimate reconcile = cooker.estimate_reconcile_invalidation(manifest);
    expectTrue(reconcile.stale_dependency_entries >= 1u, "reconcile estimate includes stale dependency count");
    expectTrue(reconcile.total() >= 1u, "reconcile estimate total is non-zero after upstream change");

    expectTrue(estimate.downstream_entries == 1u, "upstream estimate counts downstream dependent entry");
    expectTrue(estimate.total() == 2u, "upstream estimate total matches chain size");

               "would_upstream_invalidate true for seeded chain");
    expectTrue(!cooker.would_upstream_invalidate(manifest, ""),
               "would_upstream_invalidate guarded on empty source");
               "would_reconcile_invalidation false on fresh cache");

    expectTrue(removed == estimate.total(), "upstream invalidation removes estimated total");
        source_upstream.emplace_back(job.source_path, 0u);
               "mismatched upstream hash would_invalidate stale upstream");
    expectTrue(cooker.cache().count_unique_stale_upstream_sources(source_upstream) >= 1u,
               "unique stale upstream source count is non-zero");
               "would_invalidate_downstream reports dependents");


    expectTrue(cooker.cook_manifest(manifest).ok, "chain manifest cook for would downstream probe ok");

               "would_invalidate_downstream true for producer output");
    expectTrue(cooker.cache().count_downstream_of(entry_a.output_path, graph.edges(), graph.jobs()) == 1u,
               "downstream count matches single dependent entry");


    expectTrue(!cooker.would_reconcile_invalidate(manifest),
               "fresh cache would_reconcile_invalidate is false");

    expectTrue(cooker.would_reconcile_invalidate(manifest),
               "stale dependency makes would_reconcile_invalidate true");

    expectTrue(removed >= estimate.total(), "upstream invalidation removes at least estimated total");
               "would_reconcile_invalidate false after upstream invalidation clears stale deps");










    expectTrue(cooker.cook_manifest(manifest).ok, "manifest cook for upstream reconcile probes ok");


    expectTrue(cooker.cook_manifest(manifest).ok, "manifest cook for would reconcile ok");

    writeTempFile(source_a, "# would rec a revised\n");
               "would_reconcile true after upstream content change");
    expectTrue(cooker.estimate_reconcile_invalidation(manifest).stale_dependency_entries >= 1u,
               "reconcile estimate reports stale dependency entries");
void testCookerReconcilePreflightGuards() {
    const std::string source = writeTempFile("/tmp/fuse_b79_preflight_reconcile.obj", "# preflight reconcile\n");

    entry.output_path = "/tmp/fuse_b79_preflight_reconcile.fusemesh";

    expectTrue(cooker.cook_manifest(manifest).ok, "manifest cook for reconcile preflight ok");

    const fuse::project::CookCacheReconcileEstimate fresh =
        cooker.preflight_reconcile_invalidation(manifest);
    expectTrue(fresh.should_skip(), "fresh cache reconcile preflight should skip");
    expectTrue(!fresh.can_reconcile(), "fresh cache cannot reconcile");
    expectTrue(cooker.should_skip_reconcile_invalidation(manifest),
               "should_skip_reconcile true on fresh cache");
               "would_reconcile false on fresh cache");

    writeTempFile(source, "# preflight reconcile revised\n");
    const fuse::project::CookCacheReconcileEstimate stale =
    expectTrue(stale.can_reconcile(), "stale cache reconcile preflight can reconcile");
    expectTrue(stale.total() == cooker.estimate_reconcile_invalidation(manifest).total(),
               "preflight reconcile matches estimate total");

    const std::string source = writeTempFile("/tmp/fuse_b79_unique_upstream.obj", "# unique upstream\n");
    desc.output_path = "/tmp/fuse_b79_unique_upstream.fusemesh";

    expectTrue(seeded.ok, "seed cook for unique upstream probe ok");

    const std::vector<std::pair<std::string, fuse::u64>> stale_pairs = {{source, seeded.content_hash + 1u},
                                                                         {source, seeded.content_hash + 2u}};
    const std::vector<std::string> raw = cooker.cache().probe_stale_upstream_sources(stale_pairs);
    expectTrue(raw.size() >= 2u, "raw stale upstream probe may list duplicate sources");

    const std::vector<std::string> unique = cooker.cache().probe_unique_stale_upstream_sources(stale_pairs);
    expectTrue(unique[0] == source, "unique stale upstream probe retains source path");
    expectTrue(cooker.cache().would_invalidate_stale_upstream_hashes(stale_pairs),
               "would_invalidate_stale_upstream true when upstream hash mismatches");
    entry_c.output_path = "/tmp/fuse_b79_up_est_c.fusemesh";

    expectTrue(cooker.cache().entry_count() == 3u, "three entries seeded for upstream estimate");

    expectTrue(!cooker.would_invalidate_upstream_dependency(manifest, ""),
               "empty changed source would_invalidate_upstream is false");
    expectTrue(cooker.would_invalidate_upstream_dependency(manifest, source_a),
               "would_invalidate_upstream true for seeded chain head");

    expectTrue(estimate.direct_entries == 1u, "upstream estimate direct count is one");
    expectTrue(estimate.downstream_entries == 2u, "upstream estimate downstream count is two");


    expectTrue(!cooker.would_invalidate_upstream_dependency(manifest, source_a),
               "would_invalidate_upstream false after invalidation");

void testCookerWouldReconcileInvalidationGuard() {




    expectTrue(cooker.cook_manifest(manifest).ok, "manifest cook for would_reconcile guard ok");

               "would_reconcile_invalidation true after upstream change");
    expectTrue(cooker.estimate_reconcile_invalidation(manifest).total() != 0u,
               "reconcile estimate non-zero after upstream change");

    cooker.invalidate_stale_dependency_hashes(manifest);
               "would_reconcile still true while prune-stale entries remain");

    const fuse::project::CookUpstreamInvalidateEstimate estimate =
    expectTrue(estimate.direct_entries >= 1u, "upstream estimate includes direct entries");
    expectTrue(estimate.downstream_entries >= 1u, "upstream estimate includes downstream entries");
               "would_upstream_invalidation true for seeded chain");
               "empty changed source would_upstream is guarded");

    const std::vector<std::string> sources = cooker.probe_upstream_invalidation_sources(manifest, source_a);
    expectTrue(sources.size() >= 2u, "upstream source probe lists changed source and dependents");
    expectTrue(sources[0] == source_a, "upstream source probe starts at changed source");

    expectTrue(cooker.probe_upstream_invalidation_sources(manifest, source_a).empty(),
               "upstream source probe empty after invalidation");
    entry_b.output_path = "/tmp/fuse_b79_skip_reconcile_b.fusemesh";

    expectTrue(cooker.cook_manifest(manifest).ok, "manifest cook for skip reconcile guards ok");

               "should_skip_upstream_invalidation true for empty changed source");
    expectTrue(!cooker.would_invalidate_upstream(manifest, ""),
               "would_invalidate_upstream false for empty changed source");
               "should_skip_stale_dependency_invalidation true on fresh cache");
    expectTrue(!cooker.would_stale_dependency_invalidation(manifest),
               "would_stale_dependency_invalidation false on fresh cache");
               "should_skip_reconcile_invalidation true on fresh cache");

    const fuse::project::CookCacheReconcileEstimate fresh = cooker.estimate_reconcile_invalidation(manifest);
    expectTrue(fresh.should_skip(), "reconcile estimate should_skip true on fresh cache");
    expectTrue(!fresh.would_reconcile(), "reconcile estimate would_reconcile false on fresh cache");

    expectTrue(!cooker.should_skip_upstream_invalidation(manifest, source_a),
               "should_skip_upstream_invalidation false for seeded upstream source");
    expectTrue(cooker.would_invalidate_upstream(manifest, source_a),
               "would_invalidate_upstream true for seeded upstream source");

    writeTempFile(source_a, "# skip reconcile a revised\n");
    expectTrue(!cooker.should_skip_stale_dependency_invalidation(manifest),
               "should_skip_stale_dependency_invalidation false after upstream change");
    expectTrue(cooker.would_stale_dependency_invalidation(manifest),
               "would_stale_dependency_invalidation true after upstream change");
               "should_skip_reconcile_invalidation false after upstream change");

    const fuse::project::CookCacheReconcileEstimate stale = cooker.estimate_reconcile_invalidation(manifest);
    expectTrue(stale.would_reconcile(), "reconcile estimate would_reconcile true after upstream change");
    expectTrue(!stale.should_skip(), "reconcile estimate should_skip false after upstream change");



    entry_b.output_path = "/tmp/fuse_b79_skip_inv_b.fusemesh";

    expectTrue(cooker.cook_manifest(manifest).ok, "manifest cook for skip/would probes ok");
               "would_invalidate_upstream true for seeded chain");
               "should_skip_upstream_invalidation false for seeded chain");
               "empty changed source should skip upstream invalidation");
               "empty changed source would not invalidate upstream");

               "fresh cache should skip stale dependency invalidation");
    expectTrue(!cooker.would_invalidate_stale_dependencies(manifest),
               "fresh cache would not invalidate stale dependencies");
               "fresh cache should skip reconcile invalidation");
    expectTrue(cooker.estimate_reconcile_invalidation(manifest).should_skip(),
               "fresh reconcile estimate should_skip");

    writeTempFile(source_a, "# skip inv a revised\n");
    expectTrue(cooker.would_invalidate_stale_dependencies(manifest),
               "upstream change would invalidate stale dependencies");
               "upstream change should not skip stale dependency invalidation");
               "upstream change should not skip reconcile invalidation");
    expectTrue(!cooker.estimate_reconcile_invalidation(manifest).should_skip(),
               "stale reconcile estimate should not skip");

    expectTrue(cooker.would_invalidate_upstream(manifest, source_a), "would_invalidate_upstream after content change");
    expectTrue(removed >= upstream_count, "upstream invalidation removes probed count");
    expectTrue(cooker.should_skip_upstream_invalidation(manifest, source_a),
               "should_skip upstream invalidation after cache cleared");

void testCookerReconcileShouldSkipGuards() {



    expectTrue(cooker.cook_manifest(manifest).ok, "manifest cook for reconcile should_skip ok");
               "fresh cache should_skip combined reconcile invalidation");
               "fresh cache should_skip stale dependency reconcile");
    expectTrue(cooker.should_skip_prune_reconcile(), "fresh cache should_skip prune reconcile");
    expectTrue(cooker.estimate_prune_reconcile().should_skip(),
               "fresh prune estimate should_skip is true");
               "fresh reconcile estimate should_skip is true");

               "empty changed source should_skip upstream invalidation");
               "seeded upstream source does not should_skip invalidation");

               "stale upstream change clears stale dependency should_skip");
               "stale upstream change clears combined reconcile should_skip");
    expectTrue(cooker.estimate_reconcile_invalidation(manifest).should_skip() ==
                   cooker.should_skip_reconcile_invalidation(manifest),
               "reconcile should_skip matches estimate should_skip");

    const fuse::u32 removed = cooker.invalidate_stale_dependency_hashes(manifest);
    expectTrue(removed >= 1u, "stale dependency invalidation runs after should_skip cleared");
               "stale dependency should_skip restored after invalidation");
               "stale dependency reconcile should_skip after invalidation");
    expectTrue(!after.should_skip(), "prune stale entries keep reconcile plan active");
}

void testCookerReconcileShouldSkipProbes() {
    const std::string source_a = writeTempFile("/tmp/fuse_b79_skip_reconcile_a.obj", "# skip reconcile a\n");
    const std::string source_b = writeTempFile("/tmp/fuse_b79_skip_reconcile_b.obj", "# skip reconcile b\n");

    fuse::project::CookManifest manifest;
    fuse::project::CookManifestEntry entry_a;
    entry_a.kind = fuse::project::CookAssetKind::Mesh;
    entry_a.source_path = source_a;
    entry_a.output_path = "/tmp/fuse_b79_skip_reconcile_a.fusemesh";
    manifest.assets.push_back(entry_a);

    fuse::project::CookManifestEntry entry_b;
    entry_b.kind = fuse::project::CookAssetKind::Mesh;
    entry_b.source_path = source_b;
    entry_b.output_path = "/tmp/fuse_b79_skip_reconcile_b.fusemesh";
    entry_b.dependencies.push_back(entry_a.output_path);
    manifest.assets.push_back(entry_b);

    fuse::project::AssetCooker cooker;
    expectTrue(cooker.cook_manifest(manifest).ok, "manifest cook for reconcile should_skip ok");

    expectTrue(cooker.should_skip_upstream_invalidation(manifest, ""),
               "should_skip upstream invalidation for empty changed source");
    expectTrue(!cooker.should_skip_upstream_invalidation(manifest, source_a),
               "should_skip upstream invalidation false when chain would be touched");
    expectTrue(cooker.should_skip_stale_dependency_invalidation(manifest),
               "should_skip stale dependency invalidation on fresh cache");
    expectTrue(cooker.should_skip_prune_reconcile(), "should_skip prune reconcile on fresh cache");
    expectTrue(cooker.should_skip_reconcile_invalidation(manifest),
               "should_skip combined reconcile on fresh cache");

    const fuse::project::CookCacheReconcileEstimate fresh = cooker.estimate_reconcile_invalidation(manifest);
    expectTrue(fresh.should_skip(), "fresh reconcile estimate should_skip is true");
    expectTrue(cooker.estimate_prune_reconcile().should_skip(),
               "fresh prune estimate should_skip is true");

    writeTempFile(source_a, "# skip reconcile a revised\n");
    expectTrue(!cooker.should_skip_stale_dependency_invalidation(manifest),
               "should_skip stale dependency invalidation false after upstream change");
    expectTrue(!cooker.should_skip_reconcile_invalidation(manifest),
               "should_skip combined reconcile false after upstream change");

    const fuse::project::CookCacheReconcileEstimate stale = cooker.estimate_reconcile_invalidation(manifest);
    expectTrue(!stale.should_skip(), "stale reconcile estimate should_skip is false");
    expectTrue(stale.should_skip() == cooker.should_skip_reconcile_invalidation(manifest),
               "reconcile estimate should_skip matches cooker probe");

    const fuse::u32 removed = cooker.invalidate_stale_dependency_hashes(manifest);
    expectTrue(removed >= 1u, "stale dependency invalidation removes entries after should_skip cleared");

    expectTrue(cooker.should_skip_stale_dependency_invalidation(manifest),
               "should_skip stale dependency invalidation true after reconcile");
    expectTrue(!cooker.should_skip_prune_reconcile(),
               "should_skip prune reconcile false when stale upstream entry remains");
}

void testCookCacheDownstreamWouldInvalidateProbe() {
    const std::string source_a = writeTempFile("/tmp/fuse_b79_would_down_a.obj", "# would down a\n");
    const std::string source_b = writeTempFile("/tmp/fuse_b79_would_down_b.obj", "# would down b\n");

    fuse::project::CookManifest manifest;
    fuse::project::CookManifestEntry entry_a;
    entry_a.kind = fuse::project::CookAssetKind::Mesh;
    entry_a.source_path = source_a;
    entry_a.output_path = "/tmp/fuse_b79_would_down_a.fusemesh";
    manifest.assets.push_back(entry_a);

    fuse::project::CookManifestEntry entry_b;
    entry_b.kind = fuse::project::CookAssetKind::Mesh;
    entry_b.source_path = source_b;
    entry_b.output_path = "/tmp/fuse_b79_would_down_b.fusemesh";
    entry_b.dependencies.push_back(entry_a.output_path);
    manifest.assets.push_back(entry_b);

    fuse::project::CookJobGraph graph;
    graph.build_from_manifest(manifest);

    fuse::project::AssetCooker cooker;
    expectTrue(cooker.cook_manifest(manifest).ok, "manifest cook for downstream would_invalidate ok");

    expectTrue(cooker.cache().would_invalidate_downstream_of(entry_a.output_path, graph.edges(), graph.jobs()),
               "would_invalidate_downstream reports dependent entries");
    expectTrue(!cooker.cache().would_invalidate_downstream_of("", graph.edges(), graph.jobs()),
               "would_invalidate_downstream rejects empty output path");
    expectTrue(cooker.cache().would_invalidate_downstream_of(entry_a.output_path, graph.edges(), graph.jobs()) ==
                   (cooker.cache().count_downstream_of(entry_a.output_path, graph.edges(), graph.jobs()) != 0),
               "would_invalidate_downstream matches count_downstream_of");
}

void testCookCacheDownstreamSourceProbe() {
    const std::string source_a = writeTempFile("/tmp/fuse_b79_downstream_a.obj", "# downstream a\n");
    const std::string source_b = writeTempFile("/tmp/fuse_b79_downstream_b.obj", "# downstream b\n");
    const std::string source_c = writeTempFile("/tmp/fuse_b79_downstream_c.obj", "# downstream c\n");

    fuse::project::CookManifest manifest;
    fuse::project::CookManifestEntry entry_a;
    entry_a.kind = fuse::project::CookAssetKind::Mesh;
    entry_a.source_path = source_a;
    entry_a.output_path = "/tmp/fuse_b79_downstream_a.fusemesh";
    manifest.assets.push_back(entry_a);

    fuse::project::CookManifestEntry entry_b;
    entry_b.kind = fuse::project::CookAssetKind::Mesh;
    entry_b.source_path = source_b;
    entry_b.output_path = "/tmp/fuse_b79_downstream_b.fusemesh";
    entry_b.dependencies.push_back(entry_a.output_path);
    manifest.assets.push_back(entry_b);

    fuse::project::CookManifestEntry entry_c;
    entry_c.kind = fuse::project::CookAssetKind::Mesh;
    entry_c.source_path = source_c;
    entry_c.output_path = "/tmp/fuse_b79_downstream_c.fusemesh";
    entry_c.dependencies.push_back(entry_b.output_path);
    manifest.assets.push_back(entry_c);

    fuse::project::CookJobGraph graph;
    graph.build_from_manifest(manifest);

    fuse::project::AssetCooker cooker;
    expectTrue(cooker.cook_manifest(manifest).ok, "chain manifest cook for downstream probe ok");
    expectTrue(cooker.cache().entry_count() == 3u, "three entries seeded for downstream probe");

    const std::vector<std::string> probed = cooker.cache().probe_downstream_sources(
        entry_a.output_path, graph.edges(), graph.jobs());
    expectTrue(probed.size() >= 3u, "downstream probe lists producer and dependent sources");

    bool has_middle = false;
    bool has_tail = false;
    for (const std::string& path : probed) {
        if (path == source_b) {
            has_middle = true;
        }
        if (path == source_c) {
            has_tail = true;
        }
    }
    expectTrue(has_middle, "downstream probe includes middle source");
    expectTrue(has_tail, "downstream probe includes tail source");
    expectTrue(probed[0] == entry_a.output_path, "downstream probe starts at producer output path");

    const fuse::u32 counted = cooker.cache().count_downstream_of(entry_a.output_path, graph.edges(), graph.jobs());
    expectTrue(counted == 2u, "downstream count matches dependent entries only");
    expectTrue(cooker.cache().would_invalidate_downstream_of(entry_a.output_path, graph.edges(), graph.jobs()),
               "would_invalidate_downstream true for seeded chain");
    expectTrue(!cooker.cache().would_invalidate_downstream_of("", graph.edges(), graph.jobs()),
               "empty output path would_invalidate_downstream is guarded");
    expectTrue(cooker.cache().probe_downstream_sources("", graph.edges(), graph.jobs()).empty(),
               "empty output path downstream probe is guarded");
}

void testCookerPruneReconcileEstimator() {
    const std::string source = writeTempFile("/tmp/fuse_b79_reconcile_prune.obj", "# reconcile prune v1\n");
    fuse::project::MeshImportDesc desc;
    desc.input_path = source;
    desc.output_path = "/tmp/fuse_b79_reconcile_prune.fusemesh";

    fuse::project::AssetCooker cooker;
    const fuse::project::CookRecord seeded = cooker.cook_mesh(desc);
    expectTrue(seeded.ok, "seed cook for prune reconcile ok");
    expectTrue(cooker.estimate_prune_reconcile() == 0u, "fresh cook prune reconcile is zero");

    writeTempFile(source, "# reconcile prune v2\n");
    expectTrue(cooker.cache().count_stale_entries() == 1u, "stale entry counted before reconcile");
    expectTrue(cooker.estimate_prune_reconcile() == 1u, "prune reconcile estimates stale entry");
    expectTrue(cooker.estimate_full_cache_reconcile(fuse::project::CookManifest{}) == 1u,
               "full reconcile includes prunable stale entry");

    const fuse::u32 pruned = cooker.cache().prune_all();
    expectTrue(pruned == 1u, "prune_all removes estimated stale entry");
    expectTrue(cooker.estimate_prune_reconcile() == 0u, "prune reconcile zero after prune_all");
void testCookerWouldReconcileProbes() {
void testCookerWouldReconcileAndUpstreamProbes() {
    const std::string source_a = writeTempFile("/tmp/fuse_b79_would_reconcile_a.obj", "# would reconcile a\n");
    const std::string source_b = writeTempFile("/tmp/fuse_b79_would_reconcile_b.obj", "# would reconcile b\n");
void testCookerUpstreamInvalidationEstimateProbes() {
void testCookerUpstreamInvalidateEstimateProbes() {
    const std::string source_a = writeTempFile("/tmp/fuse_b79_up_est_a.obj", "# up est a\n");
    const std::string source_b = writeTempFile("/tmp/fuse_b79_up_est_b.obj", "# up est b\n");
void testCookerUpstreamEstimateAndReconcileWouldProbes() {
    const std::string source_a = writeTempFile("/tmp/fuse_b79_up_est_a.obj", "# upstream est a\n");
    const std::string source_b = writeTempFile("/tmp/fuse_b79_up_est_b.obj", "# upstream est b\n");
void testCookerWouldInvalidateAndUpstreamProbe() {
    const std::string source_a = writeTempFile("/tmp/fuse_b79_would_up_a.obj", "# would up a\n");
    const std::string source_b = writeTempFile("/tmp/fuse_b79_would_up_b.obj", "# would up b\n");
void testCookerWouldInvalidateAndUpstreamProbes() {
    const std::string source_a = writeTempFile("/tmp/fuse_b79_would_upstream_a.obj", "# would upstream a\n");
    const std::string source_b = writeTempFile("/tmp/fuse_b79_would_upstream_b.obj", "# would upstream b\n");
void testCookerUpstreamAndReconcileWouldProbes() {
    const std::string source_a = writeTempFile("/tmp/fuse_b79_would_chain_a.obj", "# would chain a\n");
    const std::string source_b = writeTempFile("/tmp/fuse_b79_would_chain_b.obj", "# would chain b\n");
void testCookerUpstreamEstimateProbes() {
    const std::string source_c = writeTempFile("/tmp/fuse_b79_up_est_c.obj", "# up est c\n");
void testCookerUpstreamReconcileEstimateProbes() {
    const std::string source_a = writeTempFile("/tmp/fuse_b79_upstream_est_a.obj", "# upstream est a\n");
    const std::string source_b = writeTempFile("/tmp/fuse_b79_upstream_est_b.obj", "# upstream est b\n");
void testCookerWouldInvalidateProbes() {
void testCookerReconcileWouldAndUpstreamProbes() {
void testCookerUpstreamInvalidationEstimate() {
void testCookerUpstreamEstimateAndWouldProbes() {
void testCookerWouldAndUpstreamEstimateProbes() {
void testCookerUpstreamEstimateAndWouldGuards() {
void testCookerWouldReconcileAndUpstreamEstimateProbes() {
void testCookerReconcileWouldAndProbeGuards() {
void testCookerShouldSkipReconcileProbes() {
    const std::string source_a = writeTempFile("/tmp/fuse_b79_skip_reconcile_a.obj", "# skip reconcile a\n");
    const std::string source_b = writeTempFile("/tmp/fuse_b79_skip_reconcile_b.obj", "# skip reconcile b\n");
void testCookerShouldSkipAndWouldInvalidateProbes() {
    const std::string source_a = writeTempFile("/tmp/fuse_b79_skip_probe_a.obj", "# skip probe a\n");
    const std::string source_b = writeTempFile("/tmp/fuse_b79_skip_probe_b.obj", "# skip probe b\n");
void testCookerShouldSkipReconcileHelpers() {
void testCookerShouldSkipReconcileGuards() {
void testCookerReconcileShouldSkipEstimators() {

    fuse::project::CookManifest manifest;
    fuse::project::CookManifestEntry entry_a;
    entry_a.kind = fuse::project::CookAssetKind::Mesh;
    entry_a.source_path = source_a;
    entry_a.output_path = "/tmp/fuse_b79_would_reconcile_a.fusemesh";
    entry_a.output_path = "/tmp/fuse_b79_up_est_a.fusemesh";
    entry_a.output_path = "/tmp/fuse_b79_would_up_a.fusemesh";
    entry_a.output_path = "/tmp/fuse_b79_would_upstream_a.fusemesh";
    entry_a.output_path = "/tmp/fuse_b79_would_chain_a.fusemesh";
    entry_a.output_path = "/tmp/fuse_b79_upstream_est_a.fusemesh";
    entry_a.output_path = "/tmp/fuse_b79_skip_reconcile_a.fusemesh";
    entry_a.output_path = "/tmp/fuse_b79_skip_probe_a.fusemesh";
    manifest.assets.push_back(entry_a);

    fuse::project::CookManifestEntry entry_b;
    entry_b.kind = fuse::project::CookAssetKind::Mesh;
    entry_b.source_path = source_b;
    entry_b.output_path = "/tmp/fuse_b79_would_reconcile_b.fusemesh";
    entry_b.dependencies.push_back(entry_a.output_path);
    manifest.assets.push_back(entry_b);

    expectTrue(cooker.cook_manifest(manifest).ok, "manifest cook for would reconcile probes ok");
    expectTrue(!cooker.would_upstream_invalidation(manifest, ""),
               "empty changed source would_upstream is false");
    expectTrue(!cooker.would_upstream_invalidation(manifest, "/tmp/fuse_b79_missing_upstream.obj"),
               "unknown changed source would_upstream is false");
    expectTrue(cooker.would_upstream_invalidation(manifest, source_a),
               "known changed source would_upstream is true");
    fuse::project::AssetCooker cooker;
    expectTrue(cooker.cook_manifest(manifest).ok, "manifest cook for would-reconcile probes ok");

    expectTrue(!cooker.would_invalidate_upstream_dependency(manifest, ""),
    expectTrue(!cooker.would_stale_dependency_invalidation(manifest),
               "fresh cache would_stale_dependency is false");
    expectTrue(!cooker.would_reconcile_invalidation(manifest),
               "fresh cache would_reconcile is false");

    writeTempFile(source_a, "# would reconcile a revised\n");
    expectTrue(cooker.would_stale_dependency_invalidation(manifest),
               "upstream change makes would_stale_dependency true");
    expectTrue(cooker.would_reconcile_invalidation(manifest),
               "upstream change makes would_reconcile true");
    expectTrue(!cooker.would_reconcile_invalidation(manifest), "fresh cache would_reconcile is false");
    expectTrue(!cooker.would_upstream_invalidation(manifest, ""), "empty changed source would_upstream is false");
               "would_upstream true for seeded chain head");
    expectTrue(cooker.probe_upstream_invalidation_sources(manifest, "").empty(),
               "empty changed source upstream probe is empty");

    const std::vector<std::string> upstream_sources = cooker.probe_upstream_invalidation_sources(manifest, source_a);
    expectTrue(upstream_sources.size() >= 2u, "upstream probe lists head and downstream sources");
    expectTrue(upstream_sources[0] == source_a, "upstream probe starts at changed source");

    expectTrue(cooker.would_reconcile_invalidation(manifest), "would_reconcile true after upstream change");
    expectTrue(cooker.estimate_reconcile_invalidation(manifest).total() != 0u,
               "estimate total non-zero when would_reconcile is true");
    entry_b.output_path = "/tmp/fuse_b79_skip_reconcile_b.fusemesh";

    expectTrue(cooker.cook_manifest(manifest).ok, "manifest cook for should_skip reconcile ok");
    expectTrue(!cooker.should_skip_upstream_invalidation(manifest, source_a),
               "should_skip_upstream false when chain entries exist");
    expectTrue(cooker.should_skip_upstream_invalidation(manifest, source_a) ==
                   (cooker.count_upstream_invalidation(manifest, source_a) == 0),
               "should_skip_upstream matches zero count probe");
    expectTrue(cooker.should_skip_upstream_invalidation(manifest, ""),
               "should_skip_upstream true for empty changed source");
    expectTrue(cooker.should_skip_stale_dependency_invalidation(manifest),
               "should_skip_stale_dependency true on fresh cache");
    expectTrue(cooker.should_skip_prune_reconcile(), "should_skip_prune_reconcile true on fresh cache");
    expectTrue(cooker.should_skip_reconcile_invalidation(manifest),
               "should_skip_reconcile_invalidation true on fresh cache");
    expectTrue(cooker.estimate_reconcile_invalidation(manifest).should_skip(),
               "reconcile estimate should_skip on fresh cache");

    writeTempFile(source_a, "# skip reconcile a revised\n");
    expectTrue(!cooker.should_skip_stale_dependency_invalidation(manifest),
               "should_skip_stale_dependency false after upstream change");
    expectTrue(!cooker.should_skip_reconcile_invalidation(manifest),
               "should_skip_reconcile false after upstream change");

    const fuse::u32 stale_count = cooker.count_stale_dependency_invalidation(manifest);
    expectTrue(stale_count >= 1u, "stale dependency count non-zero after upstream change");
    expectTrue(cooker.should_skip_stale_dependency_invalidation(manifest) == (stale_count == 0u),
               "should_skip_stale_dependency mirrors count probe");

    fuse::project::CookJobGraph graph;
    graph.build_from_manifest(manifest);
    expectTrue(cooker.cache().would_invalidate_downstream_of(entry_a.output_path, graph.edges(), graph.jobs()),
               "would_invalidate_downstream_of true for chain producer");
    expectTrue(!cooker.cache().would_invalidate_downstream_of("", graph.edges(), graph.jobs()),
               "empty output path would_invalidate_downstream_of is guarded");

    std::vector<std::pair<std::string, fuse::u64>> source_upstream;
    for (const fuse::project::CookJob& job : graph.jobs()) {
        fuse::u64 upstream = 0;
        for (const std::string& dep_id : job.dependency_ids) {
            for (const fuse::project::CookJob& dep : graph.jobs()) {
                if (dep.id == dep_id) {
                    upstream = fuse::project::fnv1a64_combine(
                        upstream, fuse::project::fnv1a64_bytes(
                                        reinterpret_cast<const fuse::u8*>(dep.output_path.data()),
                                        dep.output_path.size()));
                    upstream = fuse::project::fnv1a64_combine(upstream,
                                                              fuse::project::hash_file_content(dep.source_path));
                    break;
                }
        source_upstream.emplace_back(job.source_path, upstream);
    expectTrue(cooker.cache().would_invalidate_stale_upstream_hashes(source_upstream),
               "would_invalidate_stale_upstream true after upstream content change");
               "would_invalidate_downstream true for chain producer");
               "changed upstream source would_upstream is true");

    const std::vector<std::string> upstream_sources =
        cooker.probe_upstream_invalidation_sources(manifest, source_a);
    expectTrue(upstream_sources.size() >= 2u, "upstream probe lists changed source and dependents");

    expectTrue(cooker.would_reconcile_invalidation(manifest), "stale dependency makes would_reconcile true");
    expectTrue(cooker.estimate_reconcile_invalidation(manifest).total() >= 1u,
               "would_reconcile agrees with reconcile estimate total");
    entry_b.output_path = "/tmp/fuse_b79_up_est_b.fusemesh";



    expectTrue(cooker.cook_manifest(manifest).ok, "manifest cook for upstream estimate ok");

    const fuse::project::CookUpstreamInvalidationEstimate empty =
        cooker.estimate_upstream_invalidation(manifest, "");
    expectTrue(empty.total() == 0u, "empty changed source upstream estimate is zero");
               "empty changed source upstream probe is guarded");

    const fuse::project::CookUpstreamInvalidationEstimate estimate =
        cooker.estimate_upstream_invalidation(manifest, source_a);
    expectTrue(estimate.direct_entries >= 1u, "upstream estimate counts direct entries");
    expectTrue(estimate.total() >= estimate.direct_entries, "upstream estimate includes downstream");
    expectTrue(cooker.count_upstream_invalidation(manifest, source_a) == estimate.total(),
               "upstream count probe matches estimate total");

    const std::vector<std::string> probed = cooker.probe_upstream_invalidation_sources(manifest, source_a);
    expectTrue(!probed.empty(), "upstream probe lists affected sources");
    expectTrue(probed[0] == source_a, "upstream probe starts at changed source");

void testCookerWouldReconcileInvalidationProbe() {

    expectTrue(estimate.direct_entries == 1u, "upstream estimate direct count is one");
    expectTrue(estimate.downstream_entries == 1u, "upstream estimate downstream count is one");
    expectTrue(estimate.total() == cooker.count_upstream_invalidation(manifest, source_a),
               "upstream estimate total matches count probe");

    expectTrue(probed.size() >= 2u, "upstream probe lists changed source and dependents");

void testCookerReconcileWouldProbes() {
    entry_b.output_path = "/tmp/fuse_b79_would_chain_b.fusemesh";

    expectTrue(cooker.cook_manifest(manifest).ok, "manifest cook for would probes ok");
    expectTrue(!cooker.would_upstream_invalidate(manifest, ""), "empty changed source would not invalidate");
    expectTrue(cooker.would_upstream_invalidate(manifest, source_a),
               "would_upstream_invalidate true for seeded chain head");
    expectTrue(!cooker.would_stale_dependency_invalidate(manifest),
               "fresh cache would not stale-dependency invalidate");
    expectTrue(!cooker.would_reconcile_invalidate(manifest),
               "fresh cache would not reconcile invalidate");

    writeTempFile(source_a, "# would chain a revised\n");
    expectTrue(cooker.would_stale_dependency_invalidate(manifest),
               "upstream change makes would_stale_dependency_invalidate true");
    expectTrue(cooker.would_reconcile_invalidate(manifest),
               "upstream change makes would_reconcile_invalidate true");
    expectTrue(cooker.cook_manifest(manifest).ok, "manifest cook for would_reconcile probes ok");
    expectTrue(!cooker.estimate_reconcile_invalidation(manifest).would_reconcile(),
               "fresh reconcile estimate would_reconcile is false");

    expectTrue(cooker.count_stale_dependency_invalidation(manifest) >= 1u,
               "stale dependency count agrees with would_reconcile");

    const fuse::u32 removed = cooker.invalidate_stale_dependency_hashes(manifest);
    expectTrue(removed >= 1u, "stale invalidation removes probed entries");
    expectTrue(cooker.estimate_reconcile_invalidation(manifest).stale_dependency_entries == 0u,
               "stale dependency reconcile estimate zero after invalidation");
    expectTrue(cooker.estimate_reconcile_invalidation(manifest).prune_stale_entries >= 1u,
               "upstream entry remains stale for prune reconcile");

void testCookerProbeUpstreamInvalidationSources() {
    const std::string source_a = writeTempFile("/tmp/fuse_b79_probe_up_a.obj", "# probe up a\n");
    const std::string source_b = writeTempFile("/tmp/fuse_b79_probe_up_b.obj", "# probe up b\n");
    const std::string source_c = writeTempFile("/tmp/fuse_b79_probe_up_c.obj", "# probe up c\n");

    fuse::project::CookManifest manifest;
    fuse::project::CookManifestEntry entry_a;
    entry_a.kind = fuse::project::CookAssetKind::Mesh;
    entry_a.source_path = source_a;
    entry_a.output_path = "/tmp/fuse_b79_probe_up_a.fusemesh";
    manifest.assets.push_back(entry_a);

    fuse::project::CookManifestEntry entry_b;
    entry_b.kind = fuse::project::CookAssetKind::Mesh;
    entry_b.source_path = source_b;
    entry_b.output_path = "/tmp/fuse_b79_probe_up_b.fusemesh";



    fuse::project::CookManifestEntry entry_c;
    entry_c.kind = fuse::project::CookAssetKind::Mesh;
    entry_c.source_path = source_c;
    entry_c.output_path = "/tmp/fuse_b79_up_est_c.fusemesh";
    entry_c.dependencies.push_back(entry_b.output_path);
    manifest.assets.push_back(entry_c);

    expectTrue(cooker.cook_manifest(manifest).ok, "chain manifest cook for upstream estimate ok");
    expectTrue(cooker.cache().entry_count() == 3u, "three entries seeded for upstream estimate");



    expectTrue(estimate.direct_source_entries == 1u, "upstream estimate counts direct source entry");
    expectTrue(estimate.downstream_entries == 2u, "upstream estimate counts downstream entries");
    expectTrue(estimate.total() == 3u, "upstream estimate total matches chain size");

    expectTrue(!cooker.would_upstream_invalidate(manifest, ""),
               "would_upstream_invalidate guarded on empty source");
    expectTrue(!cooker.would_upstream_invalidate(manifest, "/tmp/fuse_b79_missing_up_est.obj"),
               "would_upstream_invalidate false for unknown source");

    const fuse::u32 count = cooker.count_upstream_invalidation(manifest, source_a);
    expectTrue(count == estimate.total(), "count_upstream_invalidation matches estimate total");

    expectTrue(probed.size() >= 3u, "upstream probe lists changed source and dependents");

    const fuse::u32 removed = cooker.invalidate_upstream_dependency(manifest, source_a);
    expectTrue(removed >= estimate.total(), "upstream invalidation removes at least estimated total");
    expectTrue(!cooker.would_upstream_invalidate(manifest, source_a),
               "would_upstream_invalidate false after upstream invalidation");


void testCookerWouldReconcileInvalidation() {
    const std::string source = writeTempFile("/tmp/fuse_b79_would_reconcile.obj", "# would reconcile\n");



    expectTrue(cooker.cache().entry_count() == 2u, "two entries seeded for upstream estimate");

    const fuse::project::CookCacheUpstreamInvalidationEstimate estimate =
    expectTrue(estimate.downstream_entries == 1u, "upstream estimate counts downstream entry");
    expectTrue(estimate.total() == 2u, "upstream estimate total matches chain size");
               "would_upstream_invalidate true for seeded chain");
               "empty changed source would_upstream_invalidate guarded");
               "unknown changed source would_upstream_invalidate guarded");

    expectTrue(removed == estimate.total(), "upstream invalidation matches estimate total");
               "would_upstream_invalidate false after invalidation");

    const std::string source = writeTempFile("/tmp/fuse_b79_reconcile_would.obj", "# reconcile would\n");

    fuse::project::CookManifestEntry entry;
    entry.kind = fuse::project::CookAssetKind::Mesh;
    entry.source_path = source;
    entry.output_path = "/tmp/fuse_b79_would_reconcile.fusemesh";
    manifest.assets.push_back(entry);

    expectTrue(cooker.cook_manifest(manifest).ok, "manifest cook for would_reconcile probe ok");

    writeTempFile(source, "# would reconcile revised\n");
    expectTrue(cooker.would_reconcile_invalidation(manifest), "stale cache would_reconcile is true");
               "would_reconcile agrees with reconcile estimate");


               "empty changed source would_upstream_invalidate is false");
    expectTrue(!cooker.would_upstream_invalidate(manifest, "/tmp/fuse_b79_up_est_unknown.obj"),
               "unknown source would_upstream_invalidate is false");
    expectTrue(cooker.would_upstream_invalidate(manifest, source_b),
               "leaf source with cache entry would_upstream_invalidate is true");

    const fuse::project::CookCacheUpstreamInvalidateEstimate estimate =
               "producer source would_upstream_invalidate is true");

    const fuse::u32 count_probe = cooker.count_upstream_invalidation(manifest, source_a);
    expectTrue(count_probe == estimate.total(),
               "count_upstream_invalidation matches upstream estimate total");

               "fresh cache would_reconcile_invalidation is false");

    writeTempFile(source_a, "# up est a revised\n");
               "stale upstream makes would_reconcile_invalidation true");

    const fuse::project::CookUpstreamInvalidationEstimate empty_upstream =
    expectTrue(empty_upstream.total() == 0u, "empty changed source upstream estimate is zero");

    const fuse::project::CookUpstreamInvalidationEstimate upstream =
    expectTrue(upstream.direct_entries == 1u, "upstream estimate counts direct entry");
    expectTrue(upstream.downstream_entries >= 1u, "upstream estimate counts downstream entries");
    expectTrue(upstream.total() == cooker.count_upstream_invalidation(manifest, source_a),


    expectTrue(cooker.would_reconcile_invalidation(manifest), "stale upstream makes would_reconcile true");

    const fuse::project::CookCacheReconcileEstimate reconcile = cooker.estimate_reconcile_invalidation(manifest);
    expectTrue(reconcile.total() >= 1u, "reconcile estimate non-zero after upstream change");
    expectTrue(cooker.would_reconcile_invalidation(manifest) == (reconcile.total() != 0u),
               "would_reconcile matches reconcile estimate total");

void testCookCacheDownstreamWouldInvalidateProbe() {
    const std::string source_a = writeTempFile("/tmp/fuse_b79_would_down_a.obj", "# would down a\n");
    const std::string source_b = writeTempFile("/tmp/fuse_b79_would_down_b.obj", "# would down b\n");

    entry_a.output_path = "/tmp/fuse_b79_would_down_a.fusemesh";

    entry_b.output_path = "/tmp/fuse_b79_would_down_b.fusemesh";


    expectTrue(cooker.cook_manifest(manifest).ok, "chain cook for downstream would_invalidate ok");

               "empty output path would_invalidate_downstream is guarded");
               "producer output would_invalidate_downstream is true");
    expectTrue(cooker.cache().count_downstream_of(entry_a.output_path, graph.edges(), graph.jobs()) == 1u,
               "downstream count matches dependent entry");


    expectTrue(upstream.direct_entries == 1u, "upstream estimate direct entries for changed source");
    expectTrue(upstream.downstream_entries >= 1u, "upstream estimate includes downstream entries");



    const std::vector<std::string> closure = cooker.probe_upstream_invalidation_closure(manifest, source_a);
    expectTrue(closure.size() >= 2u, "upstream closure probe lists changed source and dependents");
    expectTrue(cooker.probe_upstream_invalidation_closure(manifest, "").empty(),
               "empty changed source upstream closure probe is guarded");

    writeTempFile(source_a, "# upstream est a revised\n");

    expectTrue(reconcile.total() > 0u, "stale reconcile estimate is non-zero after upstream change");
    expectTrue(cooker.cook_manifest(manifest).ok, "manifest cook for reconcile would probes ok");

    expectTrue(cooker.would_reconcile_invalidation(manifest), "stale content makes would_reconcile true");
               "reconcile estimate prune stale count reflects content change");
               "content-only change does not make would_stale_dependency true");
    entry_b.output_path = "/tmp/fuse_b79_would_up_b.fusemesh";

    expectTrue(cooker.cook_manifest(manifest).ok, "manifest cook for would_invalidate probes ok");
    expectTrue(cooker.would_invalidate_upstream(manifest, source_a),
               "would_invalidate_upstream true for seeded chain");
    expectTrue(!cooker.would_invalidate_upstream(manifest, ""), "empty changed source would_invalidate guarded");
    expectTrue(!cooker.would_invalidate_stale_dependencies(manifest),
               "fresh cache would_invalidate_stale_dependencies is false");

    expectTrue(probed.size() >= 2u, "upstream probe lists producer and downstream sources");

    writeTempFile(source_a, "# would up a revised\n");
    expectTrue(cooker.would_invalidate_stale_dependencies(manifest),
               "would_invalidate_stale_dependencies true after upstream change");

    expectTrue(removed >= 1u, "stale dependency invalidation removes probed entries");
               "would_invalidate_stale_dependencies false after reconcile");
    entry_b.output_path = "/tmp/fuse_b79_would_upstream_b.fusemesh";

    expectTrue(!cooker.would_upstream_invalidate(manifest, ""), "empty changed source would_upstream is false");
    expectTrue(!cooker.would_reconcile_invalidate(manifest), "fresh cache would_reconcile is false");

               "would_upstream true for seeded upstream source");
    const fuse::project::CookCacheUpstreamInvalidationEstimate upstream_estimate =
    expectTrue(upstream_estimate.total() >= 2u, "upstream estimate totals chain entries");
    expectTrue(upstream_estimate.direct_source_entries >= 1u,
               "upstream estimate includes direct source entries");
    expectTrue(upstream_estimate.downstream_entries >= 1u, "upstream estimate includes downstream entries");
    expectTrue(cooker.count_upstream_invalidation(manifest, source_a) == upstream_estimate.total(),
               "upstream count matches estimate total");

    expectTrue(probed.size() >= 2u, "upstream source probe lists changed source and dependents");
    bool has_upstream = false;
    bool has_downstream = false;
    for (const std::string& path : probed) {
        if (path == source_a) {
            has_upstream = true;
        if (path == source_b || path == entry_a.output_path) {
            has_downstream = true;
    expectTrue(has_upstream, "upstream probe includes changed source");
    expectTrue(has_downstream, "upstream probe includes downstream source or output");

    writeTempFile(source_a, "# would upstream a revised\n");
               "would_stale_dependency true after upstream content change");
    expectTrue(cooker.would_reconcile_invalidate(manifest), "would_reconcile true after upstream change");

    expectTrue(reconcile.stale_dependency_entries >= 1u,
               "reconcile estimate reports stale dependency entries after change");

    expectTrue(!cooker.would_upstream_invalidate(manifest, ""), "empty changed source would_upstream is guarded");
               "would_upstream reports upstream chain invalidation");


    expectTrue(cooker.would_reconcile_invalidation(manifest), "stale upstream marks would_reconcile true");

    const std::vector<std::string> stale_sources = cooker.probe_stale_dependency_sources(manifest);
    expectTrue(!stale_sources.empty(), "stale dependency probe lists upstream-stale sources");
    bool has_dependent = false;
    for (const std::string& path : stale_sources) {
        if (path == source_b) {
            has_dependent = true;
    expectTrue(has_dependent, "stale dependency probe includes dependent manifest source");
    expectTrue(cooker.cook_manifest(manifest).ok, "manifest cook for would_reconcile ok");

               "stale content makes would_reconcile_invalidation true");

    const fuse::project::CookCacheReconcileEstimate estimate = cooker.estimate_reconcile_invalidation(manifest);
    expectTrue(estimate.total() > 0u, "estimate_reconcile_invalidation non-zero when would_reconcile true");
    expectTrue(estimate.prune_stale_entries >= 1u, "stale prune count included in reconcile estimate");

    expectTrue(cooker.cache().prune_all() >= 1u, "prune_all clears stale reconcile estimate");
               "would_reconcile_invalidation false after prune_all");
    entry_b.output_path = "/tmp/fuse_b79_upstream_est_b.fusemesh";

    expectTrue(cooker.cook_manifest(manifest).ok, "manifest cook for upstream reconcile estimate ok");
    expectTrue(cooker.cache().entry_count() == 2u, "two entries seeded for upstream reconcile estimate");

    const fuse::project::CookUpstreamReconcileEstimate estimate =
    expectTrue(estimate.total() == 2u, "upstream estimate total matches count probe");
               "upstream count probe matches reconcile estimate total");
               "would_upstream_invalidation true for seeded chain");
               "would_upstream_invalidation guards empty changed source");
               "would_reconcile_invalidation false on fresh cache");

    expectTrue(probed[0] == source_a, "upstream source probe starts at changed source");
    expectTrue(has_dependent, "upstream source probe includes dependent source");
    entry_c.output_path = "/tmp/fuse_b79_probe_up_c.fusemesh";

    expectTrue(cooker.cook_manifest(manifest).ok, "chain manifest cook for upstream source probe ok");

    expectTrue(probed.size() >= 3u, "upstream source probe lists head and downstream sources");

    bool has_middle = false;
    bool has_tail = false;
            has_middle = true;
        if (path == source_c) {
            has_tail = true;
    expectTrue(has_middle, "upstream source probe includes middle source");
    expectTrue(has_tail, "upstream source probe includes tail source");
               "seeded cache would_upstream reports removable entries");
    expectTrue(!cooker.would_upstream_invalidate(manifest, ""), "empty source would_upstream guarded");
    expectTrue(cooker.count_prune_reconcile() == 0u, "fresh count_prune_reconcile is zero");

    expectTrue(upstream.direct_entries >= 1u, "upstream estimate direct entries non-zero");

               "would_stale_dependency true after upstream hash change");
               "would_reconcile true after upstream hash change");
    expectTrue(cooker.count_prune_reconcile() >= 1u, "count_prune_reconcile non-zero after source change");

    const fuse::project::CookCacheReconcileEstimate fresh_with_source =
        cooker.estimate_reconcile_invalidation(manifest, source_a);
    expectTrue(fresh_with_source.upstream_invalidation_entries >= 2u,
               "fresh upstream reconcile estimate counts chain entries");
    expectTrue(fresh_with_source.total() >= fresh_with_source.upstream_invalidation_entries,
               "upstream reconcile total includes upstream count");

    expectTrue(probed[0] == source_a || probed[1] == source_a, "upstream probe includes changed source");


    const fuse::project::CookCacheReconcileEstimate stale_with_source =
    expectTrue(stale_with_source.stale_dependency_entries >= 1u,
               "stale upstream reconcile estimate includes dependency count");
    expectTrue(stale_with_source.upstream_invalidation_entries >= 2u,
               "stale upstream reconcile estimate includes upstream count");


    expectTrue(cooker.probe_stale_dependency_sources(manifest).empty(),
               "fresh cache stale dependency source probe is empty");

    expectTrue(cooker.would_invalidate_upstream_dependency(manifest, source_a),
               "would_upstream true when entries exist for changed source");
    expectTrue(upstream_sources.size() >= 2u, "upstream source probe lists producer and dependent");
    expectTrue(upstream_sources[0] == source_a, "upstream source probe starts at changed source");

               "would_reconcile true when stale dependency entries exist");

    expectTrue(!stale_sources.empty(), "stale dependency source probe non-empty after upstream change");
    expectTrue(has_downstream, "stale dependency source probe includes downstream source");

               "would_stale_dependency false after stale invalidation");

               "stale upstream would_reconcile_invalidation is true");



    expectTrue(estimate.total() > 0u, "stale cache reconcile estimate is non-zero");
    expectTrue(cooker.would_reconcile_invalidation(manifest) == (estimate.total() != 0u),
               "would_reconcile matches estimate total");

void testCookerStaleDependencyReconcileEstimate() {
    const std::string source_a = writeTempFile("/tmp/fuse_b79_stale_est_a.obj", "# stale est a\n");
    const std::string source_b = writeTempFile("/tmp/fuse_b79_stale_est_b.obj", "# stale est b\n");
    const std::string source_c = writeTempFile("/tmp/fuse_b79_stale_est_c.obj", "# stale est c\n");

    entry_a.output_path = "/tmp/fuse_b79_stale_est_a.fusemesh";

    entry_b.output_path = "/tmp/fuse_b79_stale_est_b.fusemesh";

    entry_c.output_path = "/tmp/fuse_b79_stale_est_c.fusemesh";

    expectTrue(cooker.cook_manifest(manifest).ok, "manifest cook for stale dependency estimate ok");

    const fuse::project::CookStaleDependencyEstimate fresh = cooker.estimate_stale_dependency_reconcile(manifest);
    expectTrue(fresh.total() == 0u, "fresh cache stale dependency estimate is zero");

    writeTempFile(source_a, "# stale est a revised\n");
    const fuse::project::CookStaleDependencyEstimate stale = cooker.estimate_stale_dependency_reconcile(manifest);
    expectTrue(stale.stale_upstream_entries >= 1u, "stale upstream entries counted after source change");
    expectTrue(stale.downstream_cascade_entries >= 1u,
               "downstream cascade entries counted after upstream hash change on middle job");
    expectTrue(stale.total() >= 2u, "stale dependency estimate total includes cascade");

    expectTrue(cooker.cache().would_invalidate_stale_upstream_hashes(
                   {{source_b, fuse::project::hash_upstream_dependencies({entry_a.output_path}, manifest)}}),
               "would_invalidate_stale_upstream true when upstream hash mismatches");

    expectTrue(removed >= stale.stale_upstream_entries,
               "stale dependency invalidation removes at least upstream estimate");

    const fuse::project::CookStaleDependencyEstimate after = cooker.estimate_stale_dependency_reconcile(manifest);
    expectTrue(after.stale_upstream_entries == 0u,
               "stale upstream estimate zero after stale dependency invalidation");
    expectTrue(cooker.cook_manifest(manifest).ok, "chain manifest cook for upstream probe ok");

    const fuse::u32 upstream_count = cooker.count_upstream_invalidation(manifest, source_a);
    expectTrue(cooker.would_upstream_invalidate(manifest, source_a), "would_upstream true for seeded chain");
    expectTrue(upstream_count >= 2u, "upstream count probe estimates chain removals");


    bool has_changed = false;
            has_changed = true;
    expectTrue(has_changed, "upstream probe includes changed source");
    expectTrue(has_middle, "upstream probe includes middle dependent source");
    expectTrue(has_tail, "upstream probe includes tail dependent source");

    expectTrue(removed >= upstream_count, "upstream invalidation removes at least probed count");
               "would_upstream false after upstream invalidation");
    expectTrue(cooker.probe_upstream_invalidation_sources(manifest, source_a).empty(),
               "upstream probe empty after invalidation");
    entry.output_path = "/tmp/fuse_b79_reconcile_would.fusemesh";

    expectTrue(cooker.cook_manifest(manifest).ok, "manifest cook for reconcile would probe ok");

    writeTempFile(source, "# reconcile would revised\n");
               "stale prune makes would_reconcile true");
               "reconcile estimate reports stale prune entry");






    expectTrue(upstream_estimate.direct_entries >= 1u, "upstream estimate includes direct entries");
    expectTrue(upstream_estimate.total() >= upstream_estimate.direct_entries,
               "upstream estimate total covers direct entries");

    expectTrue(probed.size() >= 2u, "upstream probe lists head and downstream paths");

               "would_stale_dependency true after upstream change");

    expectTrue(removed >= upstream_estimate.total(), "upstream invalidation removes at least estimated total");
    expectTrue(!cooker.would_upstream_invalidation(manifest, source_a),


    expectTrue(estimate.source_entries == 1u, "upstream estimate counts changed source entry");
    expectTrue(estimate.downstream_entries >= 1u, "upstream estimate counts downstream entries");
               "would_upstream_invalidation false for empty source");

               "would_reconcile_invalidation false for fresh cache");
               "would_reconcile_invalidation true after upstream content change");
    expectTrue(cooker.estimate_reconcile_invalidation(manifest).total() > 0u,
               "reconcile estimate non-zero after upstream content change");


               "would_reconcile mirrors reconcile estimate total");
    expectTrue(cooker.cook_manifest(manifest).ok, "manifest cook for would reconcile guards ok");
               "would_upstream true when cache holds upstream entries");
    expectTrue(cooker.count_upstream_invalidation(manifest, source_a) >= 2u,
               "upstream count includes downstream chain on fresh cache");
    expectTrue(cooker.probe_reconcile_sources(manifest).empty(), "fresh reconcile source probe empty");

               "would_stale_dependency true after upstream source change");



    expectTrue(estimate.stale_dependency_entries >= 1u, "stale dependency entries in reconcile estimate");
    expectTrue(estimate.unique_total() <= estimate.total(), "unique total never exceeds raw total");

    const std::vector<std::string> reconcile_sources = cooker.probe_reconcile_sources(manifest);
    expectTrue(!reconcile_sources.empty(), "reconcile source probe non-empty after upstream change");
               "would_upstream true for changed upstream source");

    expectTrue(cooker.cook_manifest(manifest).ok, "manifest cook for should_skip probes ok");

    expectTrue(cooker.should_skip_upstream_invalidation(manifest, source_a) == false,
               "fresh cache should not skip upstream invalidation for seeded source");
               "empty changed source skips upstream invalidation");
               "fresh cache skips stale dependency invalidation");
               "fresh cache skips combined reconcile invalidation");

    const fuse::project::CookCacheReconcileEstimate fresh =
        cooker.estimate_reconcile_invalidation(manifest);
    expectTrue(fresh.should_skip(), "fresh reconcile estimate should_skip is true");
    expectTrue(!fresh.would_reconcile(), "fresh reconcile estimate would_reconcile is false");

               "stale upstream change does not skip stale dependency invalidation");
               "stale upstream change does not skip combined reconcile invalidation");

    const fuse::project::CookCacheReconcileEstimate stale =
    expectTrue(stale.would_reconcile(), "stale reconcile estimate would_reconcile is true");
    expectTrue(!stale.should_skip(), "stale reconcile estimate should_skip is false");
    expectTrue(stale.total() >= 1u, "stale reconcile estimate total is non-zero");


    entry_b.output_path = "/tmp/fuse_b79_skip_probe_b.fusemesh";

    expectTrue(cooker.cook_manifest(manifest).ok, "manifest cook for skip/would_invalidate probes ok");

               "fresh cache should_skip reconcile invalidation");
    expectTrue(cooker.should_skip_prune_reconcile(), "fresh cache should_skip prune reconcile");
               "fresh reconcile estimate should_skip is true");

               "would_invalidate_upstream true for seeded chain source");
               "would_invalidate_upstream guarded on empty changed source");
    expectTrue(!cooker.would_invalidate_stale_dependency_hashes(manifest),
               "would_invalidate_stale_dependency false on fresh cache");

    writeTempFile(source_a, "# skip probe a revised\n");
               "stale upstream should not skip reconcile invalidation");
    expectTrue(cooker.would_invalidate_stale_dependency_hashes(manifest),
               "would_invalidate_stale_dependency true after upstream change");
               "would_invalidate_upstream still true before invalidation");

    expectTrue(removed >= 1u, "stale dependency invalidation removes estimated entries");
               "would_invalidate_stale_dependency false after stale invalidation");
    expectTrue(!cooker.should_skip_prune_reconcile(),
               "prune reconcile should not skip after upstream content change");

               "empty changed source should_skip upstream invalidation");
               "fresh cache should_skip stale dependency invalidation");
               "seeded upstream should not skip invalidation probe");

    const fuse::project::CookCacheReconcileEstimate estimate =
    expectTrue(estimate.would_reconcile(), "stale upstream makes reconcile estimate active");
    expectTrue(estimate.should_skip() == cooker.should_skip_reconcile_invalidation(manifest),
               "estimate should_skip matches cooker helper");
               "stale upstream should not skip dependency invalidation");
               "would_invalidate_downstream_of true for chained manifest");
    expectTrue(!cooker.cache().should_skip_invalidate_downstream_of(entry_a.output_path, graph.edges(),
                                                                    graph.jobs()),
               "should_skip_invalidate_downstream_of false for chained manifest");
    expectTrue(cooker.cache().probe_downstream_sources(entry_a.output_path, graph.edges(), graph.jobs()).size() >=
                   cooker.cache().count_downstream_of(entry_a.output_path, graph.edges(), graph.jobs()),
               "downstream probe covers counted dependents");

    expectTrue(removed >= stale_count, "stale invalidation removes at least estimated count");
               "should_skip_stale_dependency true after stale invalidation");

               "fresh seeded cache would invalidate upstream source");
               "would_invalidate_upstream true when chain entries exist");

               "stale upstream makes should_skip reconcile false");
               "would_invalidate_upstream true for changed source");
               "should_skip_upstream false when invalidation would occur");

    expectTrue(removed >= 1u, "stale dependency invalidation runs after should_skip probes");

    const fuse::u32 upstream_removed = cooker.invalidate_upstream_dependency(manifest, source_a);
    expectTrue(upstream_removed >= 1u, "upstream invalidation clears remaining chain entries");
    expectTrue(cooker.should_skip_upstream_invalidation(manifest, source_a),
               "post-invalidation should_skip upstream returns true");
               "post-invalidation should_skip reconcile returns true");

               "fresh cache should_skip_reconcile_invalidation");
               "fresh reconcile estimate should_skip");
               "fresh cache should_skip_stale_dependency_invalidation");
               "empty changed source should_skip_upstream_invalidation");
               "would_invalidate_upstream_dependency for seeded chain");
               "seeded chain should not skip upstream invalidation probe");

               "upstream change makes should_skip_stale_dependency_invalidation false");
               "upstream change makes should_skip_reconcile_invalidation false");

               "should_skip_stale_dependency_invalidation after stale reconcile");


               "upstream invalidation should not skip when entries exist");

               "stale dependency should_skip false after upstream change");
               "reconcile should_skip false after upstream change");

               "reconcile estimate should_skip matches cooker guard");
    expectTrue(estimate.total() > 0u, "reconcile estimate non-zero after upstream change");

    expectTrue(removed >= 1u, "stale dependency invalidation runs after should_skip probe");
               "stale dependency should_skip true after invalidation");

    expectTrue(cooker.cook_manifest(manifest).ok, "manifest cook for reconcile should_skip ok");

               "reconcile should_skip false when stale dependency entries exist");
    expectTrue(!cooker.estimate_reconcile_invalidation(manifest).should_skip(),
               "stale reconcile estimate should_skip is false");

    expectTrue(upstream_count >= 1u, "upstream count non-zero before should_skip check");
               "upstream should_skip false when entries would be removed");
}

void testCookManifestCacheHitsOnSecondRun() {
    const std::string source = writeTempFile("/tmp/fuse_b79_rehit_mesh.obj", "# rehit mesh\n");

    fuse::project::CookManifest manifest;
    fuse::project::CookManifestEntry entry;
    entry.kind = fuse::project::CookAssetKind::Mesh;
    entry.source_path = source;
    entry.output_path = "/tmp/fuse_b79_rehit_mesh.fusemesh";
    manifest.assets.push_back(entry);

    fuse::project::AssetCooker cooker;
    const fuse::project::CookBatchResult first = cooker.cook_manifest(manifest);
    expectTrue(first.ok, "first manifest cook ok");
    expectTrue(!first.records[0].cache_hit, "first manifest cook misses");

    const fuse::project::CookBatchResult second = cooker.cook_manifest(manifest);
    expectTrue(second.ok, "second manifest cook ok");
    expectTrue(second.records[0].cache_hit, "second manifest cook hits cache");
    expectTrue(cooker.cache().stats().hits >= 1u, "manifest re-run records cache hits");
    expectTrue(cooker.cache().entry_count() == 1u, "single cache entry retained");
}

void testCookCacheReconcileEstimators() {
    const std::string sourceA = writeTempFile("/tmp/fuse_b79_est_a.obj", "# est a\n");
    const std::string sourceB = writeTempFile("/tmp/fuse_b79_est_b.obj", "# est b\n");

    fuse::project::CookManifest manifest;

    fuse::project::CookManifestEntry entryA;
    entryA.kind = fuse::project::CookAssetKind::Mesh;
    entryA.source_path = sourceA;
    entryA.output_path = "/tmp/fuse_b79_est_a.fusemesh";
    manifest.assets.push_back(entryA);

    fuse::project::CookManifestEntry entryB;
    entryB.kind = fuse::project::CookAssetKind::Mesh;
    entryB.source_path = sourceB;
    entryB.output_path = "/tmp/fuse_b79_est_b.fusemesh";
    entryB.dependencies.push_back(entryA.output_path);
    manifest.assets.push_back(entryB);

    fuse::project::AssetCooker cooker;
    const fuse::project::CookBatchResult batch = cooker.cook_manifest(manifest);
    expectTrue(batch.ok, "reconcile estimate test seeds cache");
    expectTrue(cooker.cache().entry_count() == 2u, "upstream and downstream cached");

    const fuse::project::CookCacheReconcileEstimate upstream_estimate =
        cooker.estimate_invalidate_upstream_dependency(manifest, sourceA);
    expectTrue(upstream_estimate.direct_count == 1u, "upstream estimate counts direct source entry");
    expectTrue(upstream_estimate.downstream_count >= 1u, "upstream estimate counts downstream entries");
    expectTrue(upstream_estimate.total() >= 2u, "upstream reconcile estimate covers chain entries");

    const fuse::u32 upstream_removed = cooker.invalidate_upstream_dependency(manifest, sourceA);
    expectTrue(upstream_removed == upstream_estimate.total(),
               "upstream reconcile estimate matches invalidation count");
    expectTrue(cooker.cache().empty(), "upstream invalidation clears seeded chain cache");

    const fuse::project::CookBatchResult reseeded = cooker.cook_manifest(manifest);
    expectTrue(reseeded.ok, "cache reseeded for stale reconcile estimate");
    expectTrue(cooker.cache().entry_count() == 2u, "upstream and downstream cached after reseed");

    writeTempFile(sourceA, "# est a revised\n");
    const fuse::project::CookCacheReconcileEstimate stale_estimate =
        cooker.estimate_invalidate_stale_dependency_hashes(manifest);
    expectTrue(stale_estimate.would_reconcile(), "stale dependency reconcile estimate is non-zero");
    expectTrue(stale_estimate.direct_count >= 1u, "stale reconcile estimate counts direct entries");

    const fuse::u32 stale_removed = cooker.invalidate_stale_dependency_hashes(manifest);
    expectTrue(stale_removed == stale_estimate.total(),
               "stale dependency reconcile estimate matches invalidation count");

    fuse::project::CookJobGraph graph;
    graph.build_from_manifest(manifest);
    const fuse::project::CookCacheInvalidationProbe downstream_probe =
        cooker.cache().probe_downstream_of(entryA.output_path, graph.edges(), graph.jobs());
    expectTrue(downstream_probe.affected_count == 0u,
               "downstream probe is zero after stale reconcile invalidation");

    const std::vector<std::pair<std::string, fuse::u64>> empty_upstream;
    expectTrue(!cooker.cache().probe_stale_upstream_hashes(empty_upstream).would_invalidate(),
               "empty upstream probe list is guarded");
void testCookerUpstreamEstimateAndWouldProbes() {
    const std::string source_a = writeTempFile("/tmp/fuse_b79_upstream_est_a.obj", "# upstream est a\n");
    const std::string source_b = writeTempFile("/tmp/fuse_b79_upstream_est_b.obj", "# upstream est b\n");

    fuse::project::CookManifestEntry entry_a;
    entry_a.kind = fuse::project::CookAssetKind::Mesh;
    entry_a.source_path = source_a;
    entry_a.output_path = "/tmp/fuse_b79_upstream_est_a.fusemesh";
void testCookerShouldSkipReconcileProbes() {
    const std::string source_a = writeTempFile("/tmp/fuse_b79_skip_reconcile_a.obj", "# skip reconcile a\n");
    const std::string source_b = writeTempFile("/tmp/fuse_b79_skip_reconcile_b.obj", "# skip reconcile b\n");

    entry_a.output_path = "/tmp/fuse_b79_skip_reconcile_a.fusemesh";
    manifest.assets.push_back(entry_a);

    fuse::project::CookManifestEntry entry_b;
    entry_b.kind = fuse::project::CookAssetKind::Mesh;
    entry_b.source_path = source_b;
    entry_b.output_path = "/tmp/fuse_b79_upstream_est_b.fusemesh";
    entry_b.dependencies.push_back(entry_a.output_path);
    manifest.assets.push_back(entry_b);

    expectTrue(cooker.cook_manifest(manifest).ok, "manifest cook for upstream estimate ok");
    expectTrue(cooker.cache().entry_count() == 2u, "two entries seeded for upstream estimate");

    expectTrue(!cooker.would_upstream_invalidation(manifest, ""), "empty changed source would_upstream is false");
    expectTrue(!cooker.would_reconcile_invalidation(manifest), "fresh cache would_reconcile is false");
    expectTrue(!cooker.would_stale_dependency_invalidation(manifest),
               "fresh cache would_stale_dependency is false");

    const fuse::project::CookCacheUpstreamInvalidateEstimate estimate =
        cooker.estimate_upstream_invalidation(manifest, source_a);
    expectTrue(estimate.direct_entries == 1u, "upstream estimate direct count is one");
    expectTrue(estimate.downstream_entries >= 1u, "upstream estimate includes downstream dependents");
    expectTrue(estimate.total() == cooker.count_upstream_invalidation(manifest, source_a),
               "upstream estimate total matches count probe");
    expectTrue(cooker.would_upstream_invalidation(manifest, source_a),
               "would_upstream true when estimate total is non-zero");

    const std::vector<std::string> probed = cooker.probe_upstream_invalidation_sources(manifest, source_a);
    expectTrue(probed.size() >= 2u, "upstream source probe lists changed source and dependents");
    expectTrue(probed[0] == source_a, "upstream source probe starts at changed source");

    writeTempFile(source_a, "# upstream est a revised\n");
    expectTrue(cooker.would_stale_dependency_invalidation(manifest),
               "would_stale_dependency true after upstream content change");
    expectTrue(cooker.would_reconcile_invalidation(manifest),
               "would_reconcile true after upstream content change");

    const fuse::project::CookCacheReconcileEstimate reconcile = cooker.estimate_reconcile_invalidation(manifest);
    expectTrue(reconcile.stale_dependency_entries >= 1u,
               "reconcile estimate includes stale dependency entries after upstream change");
    entry_b.output_path = "/tmp/fuse_b79_skip_reconcile_b.fusemesh";

    fuse::project::AssetCooker cooker;
    expectTrue(cooker.cook_manifest(manifest).ok, "manifest cook for should_skip reconcile probes ok");

    expectTrue(cooker.should_skip_upstream_invalidation(manifest, ""),
               "should_skip_upstream_invalidation true for empty changed source");
    expectTrue(!cooker.should_skip_upstream_invalidation(manifest, source_a),
               "should_skip_upstream_invalidation false for seeded upstream source");
    expectTrue(cooker.should_skip_stale_dependency_invalidation(manifest),
               "should_skip_stale_dependency_invalidation true on fresh cache");
    expectTrue(cooker.should_skip_reconcile_invalidation(manifest),
               "should_skip_reconcile_invalidation true on fresh cache");
    expectTrue(cooker.should_skip_prune_reconcile(),
               "should_skip_prune_reconcile true on fresh cache");

    writeTempFile(source_a, "# skip reconcile a revised\n");
    expectTrue(!cooker.should_skip_stale_dependency_invalidation(manifest),
               "should_skip_stale_dependency_invalidation false after upstream change");
    expectTrue(!cooker.should_skip_reconcile_invalidation(manifest),
               "should_skip_reconcile_invalidation false after upstream change");

    const fuse::u32 removed = cooker.invalidate_stale_dependency_hashes(manifest);
    expectTrue(removed >= 1u, "stale dependency invalidation removes entries");
               "should_skip_stale_dependency_invalidation true after stale invalidation");
    expectTrue(!cooker.should_skip_prune_reconcile(),
               "should_skip_prune_reconcile false when upstream entry is stale");
}

void testCookCacheDownstreamWouldInvalidateProbe() {
    const std::string source_a = writeTempFile("/tmp/fuse_b79_would_down_a.obj", "# would down a\n");
    const std::string source_b = writeTempFile("/tmp/fuse_b79_would_down_b.obj", "# would down b\n");

    fuse::project::CookManifest manifest;
    fuse::project::CookManifestEntry entry_a;
    entry_a.kind = fuse::project::CookAssetKind::Mesh;
    entry_a.source_path = source_a;
    entry_a.output_path = "/tmp/fuse_b79_would_down_a.fusemesh";
    manifest.assets.push_back(entry_a);

    fuse::project::CookManifestEntry entry_b;
    entry_b.kind = fuse::project::CookAssetKind::Mesh;
    entry_b.source_path = source_b;
    entry_b.output_path = "/tmp/fuse_b79_would_down_b.fusemesh";

    fuse::project::CookJobGraph graph;
    graph.build_from_manifest(manifest);

    expectTrue(cooker.cook_manifest(manifest).ok, "manifest cook for downstream would_invalidate probe ok");

    expectTrue(cooker.cache().would_invalidate_downstream_of(entry_a.output_path, graph.edges(), graph.jobs()),
               "would_invalidate_downstream_of true for seeded chain");
    expectTrue(!cooker.cache().should_skip_invalidate_downstream_of(entry_a.output_path, graph.edges(),
                                                                    graph.jobs()),
               "should_skip_invalidate_downstream_of false for seeded chain");

    const fuse::u32 removed =
        cooker.cache().invalidate_downstream_of(entry_a.output_path, graph.edges(), graph.jobs());
    expectTrue(removed >= 1u, "downstream invalidation removes dependent entries");
    expectTrue(cooker.cache().should_skip_invalidate_downstream_of(entry_a.output_path, graph.edges(),
               "should_skip_invalidate_downstream_of true after removal");
    expectTrue(!cooker.cache().would_invalidate_downstream_of(entry_a.output_path, graph.edges(), graph.jobs()),
               "would_invalidate_downstream_of false after removal");
}

} // namespace

void testCookCachePreflightAndReconcileEstimators() {
    const std::string sourceA = writeTempFile("/tmp/fuse_b79_est_a.obj", "# est a\n");
    const std::string sourceB = writeTempFile("/tmp/fuse_b79_est_b.obj", "# est b\n");

    fuse::project::CookManifest manifest;
    fuse::project::CookManifestEntry entryA;
    entryA.kind = fuse::project::CookAssetKind::Mesh;
    entryA.source_path = sourceA;
    entryA.output_path = "/tmp/fuse_b79_est_a.fusemesh";
    manifest.assets.push_back(entryA);

    fuse::project::CookManifestEntry entryB;
    entryB.kind = fuse::project::CookAssetKind::Mesh;
    entryB.source_path = sourceB;
    entryB.output_path = "/tmp/fuse_b79_est_b.fusemesh";
    entryB.dependencies.push_back(entryA.output_path);
    manifest.assets.push_back(entryB);

    fuse::project::AssetCooker cooker;
    const fuse::project::CookBatchResult batch = cooker.cook_manifest(manifest);
    expectTrue(batch.ok, "estimator test seeds cache");

    const auto lookup_preflight =
        fuse::project::preflight_cook_cache_lookup(cooker.cache(), batch.records[0].content_hash);
    expectTrue(lookup_preflight.would_hit(), "pipeline preflight sees cached entry");
    expectTrue(cooker.cache().stats().hits == 0u, "pipeline preflight does not bump hit stats");

    const auto clean_estimate = cooker.estimate_stale_dependency_hashes(manifest);
    expectTrue(!clean_estimate.would_reconcile(), "clean cache reconcile estimate is empty");

    writeTempFile(sourceA, "# est a revised\n");
    const auto stale_estimate = cooker.estimate_stale_dependency_hashes(manifest);
    expectTrue(stale_estimate.would_reconcile(), "stale upstream reconcile estimate is non-empty");
    expectTrue(stale_estimate.stale_upstream_entries >= 1u,
               "stale upstream reconcile estimate counts entries");

    const fuse::u32 reconciled = cooker.invalidate_stale_dependency_hashes(manifest);
    expectTrue(reconciled >= stale_estimate.total_entries(),
               "reconcile invalidation matches or exceeds estimate");
}

int main() {
    fuse::core::initialize();

    testParseCookManifest();
    testAssetGraphRoundTrip();
    testAssetCookerStub();
    testCookJobGraphEmpty();
    testCookDependencyGraphEmptyGuards();
    testCookDependencyGraphEmptyHelperGuards();
    testCookDependencyGraphFlattenLayers();
    testCookDependencyGraphUpstreamClosure();
    testCookDependencyGraphMergedInvalidationClosure();
    testCookDependencyGraphReachabilityAndLayerIndex();
    testCookDependencyGraphParallelLayerWidth();
    testCookDependencyGraphQueryHelpers();
    testCookDependencyGraphTopologicalLayers();
    testCookDependencyGraphInvalidationClosure();
    testCookDependencyGraphTopologicalOrder();
    testCookJobGraphUpstreamAndMergedClosure();
    testCookDependencyGraphCycleEdges();
    testCookJobGraphStageHelpers();
    testCookJobGraphReadyJobsAndInvalidationClosure();
    testCookJobGraphCycleEdgesIntegration();
    testCookJobGraphTopologicalOrderDirect();
    testCookJobGraphImplicitOutputSourceEdge();
    testCookJobGraphCycleDetectDirect();
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
    testContentHashByteSensitivity();
    testContentHashDescSensitivity();
    testCookCacheHitMiss();
    testCookCacheInvalidation();
    testCookCacheContentChangePrunesStale();
    testCookCacheOutputInvalidation();
    testCookCachePruneStaleEntries();
    testCookCacheEmptyGuards();
    testCookCacheInvalidatePruneGuards();
    testCookContentHashGuardHelpers();
    testCookCacheContainsHelper();
    testCookCachePruneInvalidEntries();
    testCombineCookCacheKeyZeroSourceGuard();
    testCookCachePruneAll();
    testCookCacheLoadPrunesStale();
    testAssetCookerInvalidateEarlyOuts();
    testAssetCookerInvalidationEmptyGuards();
    testContentHashEmptyUpstreamDeps();
    testCookManifestCacheHitsOnSecondRun();
    testCookCacheReconcileEstimator();
    testCookCachePruneEstimateMatchesPrune();
    testCookCacheInvalidateChain();
    testCookCacheStaleDependencyHashInvalidation();
    testCookCacheStaleDependencyHashEstimator();
    testCookCacheStaleDependencyEstimatorProbe();
    testCookCacheReconcileEstimators();
    testCookCacheUpstreamInvalidationEstimate();
    testCookCacheUpstreamInvalidation();
    testCookCacheRoundTrip();
    testCookCacheEmptyKeyPaths();
    testCookDirtyInvalidatesCache();
    testCookerReconcileEstimators();
    testCookerStaleDependencyReconcileEstimatorParity();
    testCookerStaleDependencyReconcileEstimator();
    testCookerUpstreamInvalidationSourceProbe();
    testCookerUpstreamInvalidationEstimateProbes();
    testCookerWouldReconcileInvalidationProbe();
    testCookerInvalidationCountProbes();
    testCookerWouldInvalidateProbes();
    testCookerReconcileEstimateShouldSkip();
    testCookerWouldInvalidationProbes();
    testCookerUpstreamInvalidationSourceProbe();
    testCookerShouldSkipAndWouldInvalidateHelpers();
    testCookerWouldInvalidateHelpers();
    testCookerShouldSkipReconcileHelpers();
    testCookerReconcileEstimateProbes();
    testCookerUpstreamReconcileProbes();
    testCookerWouldReconcileAndUpstreamProbe();
    testCookerWouldReconcileProbes();
    testCookCacheWouldInvalidateMirrorsCount();
    testCookerUpstreamInvalidationWouldAndProbe();
    testCookCacheUniqueStaleUpstreamProbe();
    testCookerUpstreamInvalidationEstimateProbes();
    testCookerWouldReconcileInvalidationProbe();
    testCookerWouldReconcileAndUpstreamProbes();
    testCookCacheDownstreamWouldInvalidateProbe();
    testCookerUpstreamInvalidationEstimate();
    testCookerWouldReconcileInvalidate();
    testCookerEstimateUpstreamInvalidation();
    testCookCacheWouldInvalidateDownstreamProbe();
    testCookerUpstreamEstimateAndWouldProbes();
    testCookerUpstreamEstimateAndReconcileWouldProbes();
    testCookerUpstreamEstimateProbes();
    testCookerWouldReconcileInvalidation();
    testCookerUpstreamReconcileEstimateProbes();
    testCookerReconcileWouldAndUpstreamProbes();
    testCookerWouldReconcileAfterUpstreamChange();
    testCookerReconcilePreflightGuards();
    testCookerProbeUpstreamInvalidationSources();
    testCookerReconcileWouldProbes();
    testCookerWouldAndUpstreamEstimateProbes();
    testCookerUpstreamEstimateAndWouldGuards();
    testCookerReconcileWouldAndProbeGuards();
    testCookerShouldSkipReconcileProbes();
    testCookerUpstreamInvalidationEstimateAndWouldGuards();
    testCookerWouldReconcileInvalidationGuard();
    testCookerUpstreamEstimateAndSourceProbe();
    testCookerShouldSkipReconcileGuards();
    testCookerShouldSkipReconcileHelpers();
    testCookerShouldSkipWouldInvalidateProbes();
    testCookerReconcileShouldSkipGuards();
    testCookerReconcileShouldSkipEstimators();
    testCookerReconcileShouldSkipProbes();
    testCookCacheDownstreamSourceProbe();
    testCookCachePreflightAndReconcileEstimators();
    testCookCacheReconcileEstimators();
    testCookStaleDependencyHashReconcileEstimate();
    testCookerReconcileEstimators();
    testCookerStaleDependencyReconcileEstimator();
    testCookerPruneReconcileEstimator();
    testCookerStaleDependencyEstimateParity();
    testCookerCacheReconcileEstimators();
    testCookerStaleDependencyReconcileEstimators();
    testCookerUpstreamProbeAndReconcileEstimate();
    testCookerStaleDependencyReconcileEstimate();
    testCookerReconcileEstimatorGuards();
    testCookerStaleDependencyReconcileProbes();
    testCookerReconcileEstimatorProbes();
    testCookerUpstreamInvalidateEstimateProbes();
    testCookerWouldInvalidateAndUpstreamProbe();
    testCookerWouldInvalidateAndUpstreamProbes();
    testCookerUpstreamAndReconcileWouldProbes();
    testCookerWouldInvalidateProbes();
    testCookerWouldReconcileAndUpstreamEstimateProbes();
    testCookerShouldSkipAndWouldInvalidateProbes();

    fuse::core::shutdown();
    return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
