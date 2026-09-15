#include <fuse/core/init.hpp>
#include <fuse/project/cook_dependency_graph.hpp>

#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

void buildDiamond(fuse::project::CookDependencyGraph& graph) {
    graph.add_node("a");
    graph.add_node("b");
    graph.add_node("c");
    graph.add_node("d");
    expectTrue(graph.add_edge("a", "b"), "diamond a->b");
    expectTrue(graph.add_edge("a", "c"), "diamond a->c");
    expectTrue(graph.add_edge("b", "d"), "diamond b->d");
    expectTrue(graph.add_edge("c", "d"), "diamond c->d");
}

void testLayerFlattenHelpers() {
    fuse::project::CookDependencyGraph graph;
    buildDiamond(graph);

    const fuse::project::CookDependencyLayerResult layers = graph.topological_layers();
    expectTrue(layers.ok, "diamond layers ok");
    expectTrue(layers.layers.size() == 3u, "diamond has three layers");

    const std::vector<std::string> flat = fuse::project::flatten_topological_layers(layers);
    expectTrue(flat.size() == 4u, "flatten yields four nodes");
    expectTrue(flat[0] == "a", "flatten preserves root");
    expectTrue(flat[3] == "d", "flatten preserves merge node");

    const std::optional<std::size_t> root_layer = fuse::project::layer_index_for(layers, "a");
    expectTrue(root_layer.has_value(), "root layer index present");
    expectTrue(*root_layer == 0u, "root is layer zero");

    const std::optional<std::size_t> branch_layer = fuse::project::layer_index_for(layers, "b");
    expectTrue(branch_layer.has_value(), "branch layer index present");
    expectTrue(*branch_layer == 1u, "branch is layer one");

    expectTrue(!fuse::project::layer_index_for(layers, "missing").has_value(), "missing node has no layer");
    expectTrue(!fuse::project::layer_index_for(layers, "").has_value(), "empty node id has no layer");

    expectTrue(fuse::project::max_parallel_layer_width(layers) == 2u, "diamond parallel width is two");

    expectTrue(graph.layers_match_topological_order(), "layers flatten matches topological order");

    const fuse::project::CookJobGraphOrderResult order = graph.topological_order();
    expectTrue(flat == order.order, "flatten equals topo order");

    fuse::project::CookDependencyGraph empty;
    expectTrue(empty.layers_match_topological_order(), "empty graph layers match vacuously");

    fuse::project::CookDependencyLayerResult cyclic_layers;
    cyclic_layers.ok = false;
    cyclic_layers.cycle_detected = true;
    expectTrue(fuse::project::flatten_topological_layers(cyclic_layers).empty(), "cyclic layers flatten guarded");
    expectTrue(fuse::project::max_parallel_layer_width(cyclic_layers) == 0u, "cyclic layers width guarded");
}

void testTransitivePredecessors() {
    fuse::project::CookDependencyGraph graph;
    graph.add_node("a");
    graph.add_node("b");
    graph.add_node("c");
    expectTrue(graph.add_edge("a", "b"), "pred chain a->b");
    expectTrue(graph.add_edge("b", "c"), "pred chain b->c");

    const fuse::project::CookInvalidationClosureResult upstream = graph.transitive_predecessors("c");
    expectTrue(upstream.ok, "valid downstream seed for predecessors");
    expectTrue(upstream.job_ids.size() == 2u, "two upstream nodes");
    expectTrue(upstream.job_ids[0] == "a", "first upstream is root");
    expectTrue(upstream.job_ids[1] == "b", "second upstream is middle");

    const fuse::project::CookInvalidationClosureResult root_upstream = graph.transitive_predecessors("a");
    expectTrue(root_upstream.ok, "root predecessor query ok");
    expectTrue(root_upstream.job_ids.empty(), "root has no predecessors");

    const fuse::project::CookInvalidationClosureResult guarded = graph.transitive_predecessors("missing");
    expectTrue(!guarded.ok, "unknown downstream seed guarded");
    expectTrue(graph.transitive_predecessors("").ok == false, "empty downstream seed guarded");
}

void testReachabilityProbe() {
    fuse::project::CookDependencyGraph graph;
    buildDiamond(graph);

    expectTrue(graph.is_reachable_successor("a", "d"), "root reaches merge");
    expectTrue(graph.is_reachable_successor("a", "b"), "root reaches branch");
    expectTrue(!graph.is_reachable_successor("b", "c"), "sibling branches are not reachable");
    expectTrue(graph.is_reachable_successor("b", "b"), "node reaches itself when present");
    expectTrue(!graph.is_reachable_successor("b", "a"), "reverse edge not reachable");
    expectTrue(!graph.is_reachable_successor("", "d"), "empty from id guarded");
    expectTrue(!graph.is_reachable_successor("a", ""), "empty to id guarded");
    expectTrue(!graph.is_reachable_successor("missing", "d"), "unknown from id guarded");

    fuse::project::CookDependencyGraph empty;
    expectTrue(!empty.is_reachable_successor("a", "b"), "empty graph reachability guarded");
}

void testBatchInvalidationClosure() {
    fuse::project::CookDependencyGraph graph;
    graph.add_node("a");
    graph.add_node("b");
    graph.add_node("c");
    graph.add_node("d");
    expectTrue(graph.add_edge("a", "b"), "batch a->b");
    expectTrue(graph.add_edge("a", "c"), "batch a->c");
    expectTrue(graph.add_edge("b", "d"), "batch b->d");

    const fuse::project::CookInvalidationBatchResult batch =
        graph.invalidation_closure_for({"a", "missing"});
    expectTrue(batch.ok, "batch closure ok with one valid seed");
    expectTrue(batch.job_ids.size() == 3u, "batch closure merges downstream nodes");
    expectTrue(batch.job_ids[0] == "b", "batch closure sorted");
    expectTrue(batch.job_ids[2] == "d", "batch closure reaches merge");

    const fuse::project::CookInvalidationBatchResult multi_seed =
        graph.invalidation_closure_for({"b", "c"});
    expectTrue(multi_seed.ok, "multi-seed batch ok");
    expectTrue(multi_seed.job_ids.size() == 1u, "multi-seed union dedupes to merge node");
    expectTrue(multi_seed.job_ids[0] == "d", "multi-seed union reaches merge");

    const fuse::project::CookInvalidationBatchResult empty_seeds = graph.invalidation_closure_for({});
    expectTrue(empty_seeds.ok, "empty seed list is ok");
    expectTrue(empty_seeds.job_ids.empty(), "empty seed list yields empty closure");

    const fuse::project::CookInvalidationBatchResult invalid_only =
        graph.invalidation_closure_for({"missing", ""});
    expectTrue(!invalid_only.ok, "all-invalid seed list guarded");
    expectTrue(invalid_only.job_ids.empty(), "all-invalid seed list yields empty ids");

    fuse::project::CookDependencyGraph empty;
    const fuse::project::CookInvalidationBatchResult empty_graph = empty.invalidation_closure_for({"a"});
    expectTrue(!empty_graph.ok, "empty graph batch closure guarded");
}

void testEmptyGraphEarlyOuts() {
    fuse::project::CookDependencyGraph graph;
    expectTrue(graph.empty(), "default graph empty");

    const fuse::project::CookDependencyLayerResult layers = graph.topological_layers();
    expectTrue(layers.ok, "empty graph layers ok");
    expectTrue(layers.layers.empty(), "empty graph yields no layers");
    expectTrue(fuse::project::flatten_topological_layers(layers).empty(), "empty layers flatten empty");
    expectTrue(fuse::project::max_parallel_layer_width(layers) == 0u, "empty layers width zero");

    const fuse::project::CookInvalidationClosureResult downstream = graph.transitive_successors("a");
    expectTrue(!downstream.ok, "empty graph downstream guarded");

    const fuse::project::CookInvalidationClosureResult upstream = graph.transitive_predecessors("a");
    expectTrue(!upstream.ok, "empty graph upstream guarded");

    expectTrue(!graph.is_reachable_successor("a", "b"), "empty graph reachability false");

    graph.add_node("");
    expectTrue(graph.empty(), "empty node id does not populate graph");
}

} // namespace

int main() {
    fuse::core::initialize();

    testLayerFlattenHelpers();
    testTransitivePredecessors();
    testReachabilityProbe();
    testBatchInvalidationClosure();
    testEmptyGraphEarlyOuts();

    fuse::core::shutdown();
    return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
