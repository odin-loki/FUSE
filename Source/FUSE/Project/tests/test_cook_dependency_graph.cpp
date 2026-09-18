#include <fuse/core/init.hpp>
#include <fuse/project/cook_dependency_graph.hpp>
#include <fuse/project/cook_job_graph.hpp>
#include <fuse/project/cook_manifest.hpp>

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

void testTopologicalOrderValidation() {
    fuse::project::CookDependencyGraph graph;
    expectTrue(fuse::project::is_valid_topological_order(graph, {}), "empty graph accepts empty order");
    expectTrue(!fuse::project::is_valid_topological_order(graph, {"orphan"}),
               "empty graph rejects non-empty order");

    graph.add_node("a");
    graph.add_node("b");
    graph.add_node("c");
    expectTrue(graph.add_edge("a", "b"), "validation test a->b");
    expectTrue(graph.add_edge("b", "c"), "validation test b->c");

    const fuse::project::CookJobGraphOrderResult order = graph.topological_order();
    expectTrue(fuse::project::is_valid_topological_order(graph, order.order), "computed order validates");
    expectTrue(!fuse::project::is_valid_topological_order(graph, {"c", "b", "a"}),
               "reverse order fails validation");
    expectTrue(!fuse::project::is_valid_topological_order(graph, {"a", "b"}),
               "partial order fails validation");
    expectTrue(!fuse::project::is_valid_topological_order(graph, {"a", "b", "c", "a"}),
               "duplicate node fails validation");
    expectTrue(!fuse::project::is_valid_topological_order(graph, {"a", "b", "missing"}),
               "unknown node fails validation");

    fuse::project::CookDependencyGraph cyclic;
    cyclic.add_node("x");
    cyclic.add_node("y");
    expectTrue(cyclic.add_edge("x", "y"), "cycle validation x->y");
    expectTrue(cyclic.add_edge("y", "x"), "cycle validation y->x");
    expectTrue(!fuse::project::is_valid_topological_order(cyclic, {"x", "y"}),
               "cyclic graph rejects every order");
}

void testDegreeAndIsolatedNodes() {
    fuse::project::CookDependencyGraph graph;
    expectTrue(graph.in_degree("a") == 0u, "empty graph in-degree guarded");
    expectTrue(graph.out_degree("a") == 0u, "empty graph out-degree guarded");
    expectTrue(graph.isolated_nodes().empty(), "empty graph isolated nodes guarded");

    graph.add_node("solo");
    graph.add_node("a");
    graph.add_node("b");
    graph.add_node("c");
    expectTrue(graph.add_edge("a", "b"), "degree test a->b");
    expectTrue(graph.add_edge("a", "c"), "degree test a->c");
    expectTrue(graph.add_edge("b", "c"), "degree test b->c");

    expectTrue(graph.in_degree("a") == 0u, "root in-degree is zero");
    expectTrue(graph.out_degree("a") == 2u, "root out-degree is two");
    expectTrue(graph.in_degree("c") == 2u, "merge in-degree is two");
    expectTrue(graph.out_degree("c") == 0u, "leaf out-degree is zero");
    expectTrue(graph.in_degree("missing") == 0u, "unknown in-degree guarded");
    expectTrue(graph.out_degree("") == 0u, "empty id out-degree guarded");

    const std::vector<std::string> isolated = graph.isolated_nodes();
    expectTrue(isolated.size() == 1u, "one isolated node");
    expectTrue(isolated[0] == "solo", "isolated node is solo");
}

void testInvalidationBundle() {
    fuse::project::CookDependencyGraph graph;
    graph.add_node("a");
    graph.add_node("b");
    graph.add_node("c");
    expectTrue(graph.add_edge("a", "b"), "bundle a->b");
    expectTrue(graph.add_edge("b", "c"), "bundle b->c");

    const fuse::project::CookInvalidationClosureResult bundle = graph.invalidation_bundle("a");
    expectTrue(bundle.ok, "valid seed bundle ok");
    expectTrue(bundle.job_ids.size() == 3u, "bundle includes seed and downstream");
    expectTrue(bundle.job_ids[0] == "a", "bundle starts with seed");
    expectTrue(bundle.job_ids[1] == "b", "bundle includes first downstream");
    expectTrue(bundle.job_ids[2] == "c", "bundle includes second downstream");

    const fuse::project::CookInvalidationClosureResult leaf_bundle = graph.invalidation_bundle("c");
    expectTrue(leaf_bundle.ok, "leaf bundle ok");
    expectTrue(leaf_bundle.job_ids.size() == 1u, "leaf bundle is seed only");
    expectTrue(leaf_bundle.job_ids[0] == "c", "leaf bundle seed preserved");

    expectTrue(!graph.invalidation_bundle("missing").ok, "unknown seed bundle guarded");
    expectTrue(!graph.invalidation_bundle("").ok, "empty seed bundle guarded");

    fuse::project::CookDependencyGraph empty;
    expectTrue(!empty.invalidation_bundle("a").ok, "empty graph bundle guarded");
}

void testMergedInvalidationBundle() {
    fuse::project::CookDependencyGraph graph;
    graph.add_node("a");
    graph.add_node("b");
    graph.add_node("c");
    graph.add_node("d");
    expectTrue(graph.add_edge("a", "b"), "merged bundle a->b");
    expectTrue(graph.add_edge("a", "c"), "merged bundle a->c");
    expectTrue(graph.add_edge("b", "d"), "merged bundle b->d");

    const fuse::project::CookInvalidationClosureResult merged = graph.merged_invalidation_bundle({"b", "c"});
    expectTrue(merged.ok, "multi-seed merged bundle ok");
    expectTrue(merged.job_ids.size() == 3u, "merged bundle includes seeds and shared downstream");
    expectTrue(merged.job_ids[0] == "b", "first seed in merged bundle");
    expectTrue(merged.job_ids[1] == "c", "second seed in merged bundle");
    expectTrue(merged.job_ids[2] == "d", "shared downstream in merged bundle");

    expectTrue(!graph.merged_invalidation_bundle({}).ok, "empty seed list guarded");
    expectTrue(!graph.merged_invalidation_bundle({"missing"}).ok, "unknown seed guarded");
}

void testClearResetsGraph() {
    fuse::project::CookDependencyGraph graph;
    graph.add_node("a");
    graph.add_node("b");
    expectTrue(graph.add_edge("a", "b"), "clear test edge");
    expectTrue(!graph.empty(), "graph populated before clear");

    graph.clear();
    expectTrue(graph.empty(), "clear yields empty graph");
    expectTrue(graph.node_count() == 0u, "clear resets node count");
    expectTrue(graph.edge_count() == 0u, "clear resets edge count");
    expectTrue(graph.topological_order().order.empty(), "clear resets topo order");
    expectTrue(!graph.invalidation_bundle("a").ok, "clear resets invalidation bundle guard");
}

void testCookJobGraphTopologicalValidation() {
    const std::string sourceA = writeTempFile("/tmp/fuse_b79_validate_a.obj", "# validate a\n");
    const std::string sourceB = writeTempFile("/tmp/fuse_b79_validate_b.obj", "# validate b\n");

    fuse::project::CookManifest manifest;

    fuse::project::CookManifestEntry entryA;
    entryA.kind = fuse::project::CookAssetKind::Mesh;
    entryA.source_path = sourceA;
    entryA.output_path = "/tmp/fuse_b79_validate_a.fusemesh";
    manifest.assets.push_back(entryA);

    fuse::project::CookManifestEntry entryB;
    entryB.kind = fuse::project::CookAssetKind::Mesh;
    entryB.source_path = sourceB;
    entryB.output_path = "/tmp/fuse_b79_validate_b.fusemesh";
    entryB.dependencies.push_back(entryA.output_path);
    manifest.assets.push_back(entryB);

    fuse::project::CookJobGraph graph;
    graph.build_from_manifest(manifest);

    const fuse::project::CookJobGraphOrderResult order = graph.topological_order();
    expectTrue(graph.is_valid_topological_order(order.order), "job graph validates its topo order");
    expectTrue(!graph.is_valid_topological_order({entryB.output_path, entryA.output_path}),
               "job graph rejects reversed order");

    fuse::project::CookJobGraph empty_graph;
    expectTrue(empty_graph.is_valid_topological_order({}), "empty job graph accepts empty order");
    expectTrue(!empty_graph.is_valid_topological_order({entryA.output_path}),
               "empty job graph rejects non-empty order");
}

void testCookJobGraphInvalidationBundle() {
    const std::string sourceA = writeTempFile("/tmp/fuse_b79_bundle_a.obj", "# bundle a\n");
    const std::string sourceB = writeTempFile("/tmp/fuse_b79_bundle_b.obj", "# bundle b\n");
    const std::string sourceC = writeTempFile("/tmp/fuse_b79_bundle_c.obj", "# bundle c\n");

    fuse::project::CookManifest manifest;

    fuse::project::CookManifestEntry entryA;
    entryA.kind = fuse::project::CookAssetKind::Mesh;
    entryA.source_path = sourceA;
    entryA.output_path = "/tmp/fuse_b79_bundle_a.fusemesh";
    manifest.assets.push_back(entryA);

    fuse::project::CookManifestEntry entryB;
    entryB.kind = fuse::project::CookAssetKind::Mesh;
    entryB.source_path = sourceB;
    entryB.output_path = "/tmp/fuse_b79_bundle_b.fusemesh";
    entryB.dependencies.push_back(entryA.output_path);
    manifest.assets.push_back(entryB);

    fuse::project::CookManifestEntry entryC;
    entryC.kind = fuse::project::CookAssetKind::Mesh;
    entryC.source_path = sourceC;
    entryC.output_path = "/tmp/fuse_b79_bundle_c.fusemesh";
    entryC.dependencies.push_back(entryB.output_path);
    manifest.assets.push_back(entryC);

    fuse::project::CookJobGraph graph;
    graph.build_from_manifest(manifest);

    const fuse::project::CookInvalidationClosureResult bundle =
        graph.invalidation_bundle(entryA.output_path);
    expectTrue(bundle.ok, "job graph invalidation bundle ok");
    expectTrue(bundle.job_ids.size() == 3u, "job graph bundle includes all three jobs");
    expectTrue(bundle.job_ids[0] == entryA.output_path, "job graph bundle includes seed");

    const fuse::project::CookInvalidationClosureResult merged =
        graph.merged_invalidation_bundle({entryA.output_path, entryB.output_path});
    expectTrue(merged.ok, "job graph merged bundle ok");
    expectTrue(merged.job_ids.size() == 3u, "job graph merged bundle dedupes overlap");

    fuse::project::CookJobGraph empty_graph;
    expectTrue(!empty_graph.invalidation_bundle(entryA.output_path).ok, "empty job graph bundle guarded");
    expectTrue(!empty_graph.merged_invalidation_bundle({entryA.output_path}).ok,
               "empty job graph merged bundle guarded");
}

} // namespace

int main() {
    fuse::core::initialize();

    testTopologicalOrderValidation();
    testDegreeAndIsolatedNodes();
    testInvalidationBundle();
    testMergedInvalidationBundle();
    testClearResetsGraph();
    testCookJobGraphTopologicalValidation();
    testCookJobGraphInvalidationBundle();

    fuse::core::shutdown();
    return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
