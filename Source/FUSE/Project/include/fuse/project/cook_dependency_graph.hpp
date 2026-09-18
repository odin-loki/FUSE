#pragma once

#include <fuse/types.hpp>

#include <string>
#include <vector>

namespace fuse::project {

/// Directed edge: `from_job_id` must complete before `to_job_id` runs.
struct CookJobDependencyEdge {
    std::string from_job_id;
    std::string to_job_id;
};

/// Topological ordering result — Kahn with stable tie-breaking; empty order when cyclic.
struct CookJobGraphOrderResult {
    std::vector<std::string> order;
    bool cycle_detected = false;
    bool ok = true;
};

/// Cycle-edge probe — back edges found by DFS when the graph is not a DAG.
struct CookDependencyCycleResult {
    bool cycle_detected = false;
    std::vector<CookJobDependencyEdge> cycle_edges;
};

/// Parallel execution layers — each inner vector is a batch of nodes with satisfied predecessors.
struct CookDependencyLayerResult {
    std::vector<std::vector<std::string>> layers;
    bool cycle_detected = false;
    bool ok = true;
};

/// Transitive downstream closure for cache invalidation — empty when the seed node is unknown.
struct CookInvalidationClosureResult {
    bool ok = true;
    std::vector<std::string> job_ids;
};

/// Cook job dependency graph — topo sort, cycle-edge detection, empty-graph guards (B7.9 deepen).
class CookDependencyGraph {
public:
    void clear();

    [[nodiscard]] bool empty() const { return m_nodes.empty(); }
    [[nodiscard]] std::size_t node_count() const { return m_nodes.size(); }
    [[nodiscard]] std::size_t edge_count() const { return m_edges.size(); }

    /// Register a node; no-op when `node_id` is empty or already present.
    void add_node(const std::string& node_id);

    /// Add a directed edge; returns false for empty ids, self-loops, unknown nodes, or duplicates.
    [[nodiscard]] bool add_edge(const std::string& from_id, const std::string& to_id);

    [[nodiscard]] bool has_node(const std::string& node_id) const;
    [[nodiscard]] bool has_edge(const std::string& from_id, const std::string& to_id) const;

    [[nodiscard]] std::vector<std::string> predecessors(const std::string& node_id) const;
    [[nodiscard]] std::vector<std::string> successors(const std::string& node_id) const;
    [[nodiscard]] std::vector<std::string> roots() const;
    [[nodiscard]] std::vector<std::string> leaves() const;

    [[nodiscard]] const std::vector<std::string>& nodes() const { return m_nodes; }
    [[nodiscard]] const std::vector<CookJobDependencyEdge>& edges() const { return m_edges; }

    [[nodiscard]] CookJobGraphOrderResult topological_order() const;
    [[nodiscard]] CookDependencyLayerResult topological_layers() const;
    [[nodiscard]] bool has_cycle() const;
    [[nodiscard]] CookDependencyCycleResult detect_cycle_edges() const;

    /// All nodes reachable along outgoing edges from `from_job_id` (invalidation guard on empty/unknown seeds).
    [[nodiscard]] CookInvalidationClosureResult transitive_successors(const std::string& from_job_id) const;

    /// All nodes reachable along incoming edges to `to_job_id` — upstream invalidation guard on empty/unknown seeds.
    [[nodiscard]] CookInvalidationClosureResult transitive_predecessors(const std::string& to_job_id) const;

    /// Union of downstream closures for each seed — guarded on empty graph / empty seed list / unknown seeds.
    [[nodiscard]] CookInvalidationClosureResult merged_invalidation_closure(
        const std::vector<std::string>& from_job_ids) const;

    /// True when `to_id` is reachable from `from_id` along directed edges; guarded on empty/unknown ids.
    [[nodiscard]] bool is_reachable(const std::string& from_id, const std::string& to_id) const;

    /// Zero-based layer index for `node_id`; -1 when unknown, graph empty, or cyclic.
    [[nodiscard]] s32 topological_layer_index(const std::string& node_id) const;

    /// Maximum parallel batch size across topological layers; 0 when graph is empty or cyclic.
    [[nodiscard]] std::size_t parallel_layer_width() const;

private:
    [[nodiscard]] bool has_node_(const std::string& node_id) const;

    std::vector<std::string> m_nodes;
    std::vector<CookJobDependencyEdge> m_edges;
};

/// Flatten layer batches into a single topological order — empty when layers are cyclic or absent.
[[nodiscard]] std::vector<std::string> flatten_topological_layers(const CookDependencyLayerResult& layers);

} // namespace fuse::project
