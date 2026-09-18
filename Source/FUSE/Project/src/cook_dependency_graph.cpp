#include <fuse/project/cook_dependency_graph.hpp>

#include <algorithm>
#include <queue>
#include <unordered_map>
#include <unordered_set>

namespace fuse::project {

namespace {

std::unordered_map<std::string, std::vector<std::string>> build_adjacency(
    const std::vector<std::string>& nodes, const std::vector<CookJobDependencyEdge>& edges) {
    std::unordered_map<std::string, std::vector<std::string>> adjacency;
    for (const std::string& node : nodes) {
        adjacency[node] = {};
    }
    for (const CookJobDependencyEdge& edge : edges) {
        if (adjacency.find(edge.from_job_id) != adjacency.end() &&
            adjacency.find(edge.to_job_id) != adjacency.end()) {
            adjacency[edge.from_job_id].push_back(edge.to_job_id);
        }
    }
    return adjacency;
}

std::unordered_map<std::string, u32> build_indegree(
    const std::vector<std::string>& nodes, const std::vector<CookJobDependencyEdge>& edges) {
    std::unordered_map<std::string, u32> indegree;
    for (const std::string& node : nodes) {
        indegree[node] = 0;
    }
    for (const CookJobDependencyEdge& edge : edges) {
        if (indegree.find(edge.from_job_id) != indegree.end() &&
            indegree.find(edge.to_job_id) != indegree.end()) {
            ++indegree[edge.to_job_id];
        }
    }
    return indegree;
}

} // namespace

void CookDependencyGraph::clear() {
    m_nodes.clear();
    m_edges.clear();
}

void CookDependencyGraph::add_node(const std::string& node_id) {
    if (node_id.empty()) {
        return;
    }
    if (has_node_(node_id)) {
        return;
    }
    m_nodes.push_back(node_id);
}

bool CookDependencyGraph::has_node_(const std::string& node_id) const {
    return std::find(m_nodes.begin(), m_nodes.end(), node_id) != m_nodes.end();
}

bool CookDependencyGraph::has_node(const std::string& node_id) const {
    return has_node_(node_id);
}

bool CookDependencyGraph::has_edge(const std::string& from_id, const std::string& to_id) const {
    if (from_id.empty() || to_id.empty()) {
        return false;
    }
    return std::find_if(m_edges.begin(), m_edges.end(), [&](const CookJobDependencyEdge& edge) {
               return edge.from_job_id == from_id && edge.to_job_id == to_id;
           }) != m_edges.end();
}

std::vector<std::string> CookDependencyGraph::predecessors(const std::string& node_id) const {
    if (node_id.empty() || !has_node_(node_id)) {
        return {};
    }

    std::vector<std::string> preds;
    for (const CookJobDependencyEdge& edge : m_edges) {
        if (edge.to_job_id == node_id) {
            preds.push_back(edge.from_job_id);
        }
    }
    std::sort(preds.begin(), preds.end());
    preds.erase(std::unique(preds.begin(), preds.end()), preds.end());
    return preds;
}

std::vector<std::string> CookDependencyGraph::successors(const std::string& node_id) const {
    if (node_id.empty() || !has_node_(node_id)) {
        return {};
    }

    std::vector<std::string> succs;
    for (const CookJobDependencyEdge& edge : m_edges) {
        if (edge.from_job_id == node_id) {
            succs.push_back(edge.to_job_id);
        }
    }
    std::sort(succs.begin(), succs.end());
    succs.erase(std::unique(succs.begin(), succs.end()), succs.end());
    return succs;
}

std::vector<std::string> CookDependencyGraph::roots() const {
    if (m_nodes.empty()) {
        return {};
    }

    const std::unordered_map<std::string, u32> indegree = build_indegree(m_nodes, m_edges);
    std::vector<std::string> roots;
    for (const std::string& node : m_nodes) {
        const auto it = indegree.find(node);
        if (it != indegree.end() && it->second == 0) {
            roots.push_back(node);
        }
    }
    std::sort(roots.begin(), roots.end());
    return roots;
}

std::vector<std::string> CookDependencyGraph::leaves() const {
    if (m_nodes.empty()) {
        return {};
    }

    const std::unordered_map<std::string, std::vector<std::string>> adjacency =
        build_adjacency(m_nodes, m_edges);
    std::vector<std::string> leaves;
    for (const std::string& node : m_nodes) {
        const auto it = adjacency.find(node);
        if (it != adjacency.end() && it->second.empty()) {
            leaves.push_back(node);
        }
    }
    std::sort(leaves.begin(), leaves.end());
    return leaves;
}

bool CookDependencyGraph::add_edge(const std::string& from_id, const std::string& to_id) {
    if (from_id.empty() || to_id.empty() || from_id == to_id) {
        return false;
    }
    if (!has_node_(from_id) || !has_node_(to_id)) {
        return false;
    }

    const auto duplicate = std::find_if(m_edges.begin(), m_edges.end(), [&](const CookJobDependencyEdge& edge) {
        return edge.from_job_id == from_id && edge.to_job_id == to_id;
    });
    if (duplicate != m_edges.end()) {
        return false;
    }

    m_edges.push_back({from_id, to_id});
    return true;
}

CookJobGraphOrderResult CookDependencyGraph::topological_order() const {
    CookJobGraphOrderResult result;

    if (m_nodes.empty()) {
        return result;
    }

    std::unordered_map<std::string, u32> indegree;
    std::unordered_map<std::string, std::vector<std::string>> adjacency;

    for (const std::string& node : m_nodes) {
        indegree[node] = 0;
        adjacency[node] = {};
    }

    for (const CookJobDependencyEdge& edge : m_edges) {
        if (indegree.find(edge.from_job_id) == indegree.end() ||
            indegree.find(edge.to_job_id) == indegree.end()) {
            continue;
        }
        adjacency[edge.from_job_id].push_back(edge.to_job_id);
        ++indegree[edge.to_job_id];
    }

    std::vector<std::string> queue;
    for (const auto& pair : indegree) {
        if (pair.second == 0) {
            queue.push_back(pair.first);
        }
    }
    std::sort(queue.begin(), queue.end());

    while (!queue.empty()) {
        const std::string current = queue.front();
        queue.erase(queue.begin());
        result.order.push_back(current);

        for (const std::string& next : adjacency[current]) {
            auto it = indegree.find(next);
            if (it == indegree.end()) {
                continue;
            }
            if (--it->second == 0) {
                queue.push_back(next);
                std::sort(queue.begin(), queue.end());
            }
        }
    }

    if (result.order.size() != m_nodes.size()) {
        result.order.clear();
        result.cycle_detected = true;
        result.ok = false;
    }

    return result;
}

CookDependencyLayerResult CookDependencyGraph::topological_layers() const {
    CookDependencyLayerResult result;

    if (m_nodes.empty()) {
        return result;
    }

    std::unordered_map<std::string, u32> indegree = build_indegree(m_nodes, m_edges);
    const std::unordered_map<std::string, std::vector<std::string>> adjacency =
        build_adjacency(m_nodes, m_edges);

    std::unordered_set<std::string> scheduled;
    while (scheduled.size() < m_nodes.size()) {
        std::vector<std::string> layer;
        for (const std::string& node : m_nodes) {
            if (scheduled.find(node) != scheduled.end()) {
                continue;
            }
            const auto indegree_it = indegree.find(node);
            if (indegree_it != indegree.end() && indegree_it->second == 0) {
                layer.push_back(node);
            }
        }

        if (layer.empty()) {
            result.layers.clear();
            result.cycle_detected = true;
            result.ok = false;
            return result;
        }

        std::sort(layer.begin(), layer.end());
        for (const std::string& node : layer) {
            scheduled.insert(node);
        }

        for (const std::string& node : layer) {
            const auto adj_it = adjacency.find(node);
            if (adj_it == adjacency.end()) {
                continue;
            }
            for (const std::string& next : adj_it->second) {
                auto it = indegree.find(next);
                if (it != indegree.end() && it->second > 0) {
                    --it->second;
                }
            }
        }

        result.layers.push_back(std::move(layer));
    }

    return result;
}

bool CookDependencyGraph::has_cycle() const {
    if (m_nodes.empty()) {
        return false;
    }
    return topological_order().cycle_detected;
}

CookInvalidationClosureResult CookDependencyGraph::transitive_successors(
    const std::string& from_job_id) const {
    CookInvalidationClosureResult result;

    if (from_job_id.empty() || m_nodes.empty() || !has_node_(from_job_id)) {
        result.ok = false;
        return result;
    }

    const std::unordered_map<std::string, std::vector<std::string>> adjacency =
        build_adjacency(m_nodes, m_edges);

    std::queue<std::string> queue;
    std::unordered_set<std::string> visited;
    queue.push(from_job_id);
    visited.insert(from_job_id);

    while (!queue.empty()) {
        const std::string current = queue.front();
        queue.pop();

        const auto adj_it = adjacency.find(current);
        if (adj_it != adjacency.end()) {
            for (const std::string& next : adj_it->second) {
                if (visited.insert(next).second) {
                    result.job_ids.push_back(next);
                    queue.push(next);
                }
            }
        }
    }

    std::sort(result.job_ids.begin(), result.job_ids.end());
    return result;
}

CookInvalidationClosureResult CookDependencyGraph::transitive_predecessors(
    const std::string& to_job_id) const {
    CookInvalidationClosureResult result;

    if (to_job_id.empty() || m_nodes.empty() || !has_node_(to_job_id)) {
        result.ok = false;
        return result;
    }

    std::unordered_map<std::string, std::vector<std::string>> reverse_adjacency;
    for (const std::string& node : m_nodes) {
        reverse_adjacency[node] = {};
    }
    for (const CookJobDependencyEdge& edge : m_edges) {
        if (reverse_adjacency.find(edge.from_job_id) != reverse_adjacency.end() &&
            reverse_adjacency.find(edge.to_job_id) != reverse_adjacency.end()) {
            reverse_adjacency[edge.to_job_id].push_back(edge.from_job_id);
        }
    }

    std::queue<std::string> queue;
    std::unordered_set<std::string> visited;
    queue.push(to_job_id);
    visited.insert(to_job_id);

    while (!queue.empty()) {
        const std::string current = queue.front();
        queue.pop();

        const auto adj_it = reverse_adjacency.find(current);
        if (adj_it != reverse_adjacency.end()) {
            for (const std::string& prev : adj_it->second) {
                if (visited.insert(prev).second) {
                    result.job_ids.push_back(prev);
                    queue.push(prev);
                }
            }
        }
    }

    std::sort(result.job_ids.begin(), result.job_ids.end());
    return result;
}

CookInvalidationClosureResult CookDependencyGraph::merged_invalidation_closure(
    const std::vector<std::string>& from_job_ids) const {
    CookInvalidationClosureResult result;

    if (m_nodes.empty() || from_job_ids.empty()) {
        result.ok = false;
        return result;
    }

    std::unordered_set<std::string> merged;
    for (const std::string& seed : from_job_ids) {
        if (seed.empty() || !has_node_(seed)) {
            result.ok = false;
            result.job_ids.clear();
            return result;
        }

        const CookInvalidationClosureResult downstream = transitive_successors(seed);
        if (!downstream.ok) {
            result.ok = false;
            result.job_ids.clear();
            return result;
        }

        for (const std::string& job_id : downstream.job_ids) {
            merged.insert(job_id);
        }
    }

    result.job_ids.assign(merged.begin(), merged.end());
    std::sort(result.job_ids.begin(), result.job_ids.end());
    return result;
}

bool CookDependencyGraph::is_reachable(const std::string& from_id, const std::string& to_id) const {
    if (from_id.empty() || to_id.empty() || m_nodes.empty()) {
        return false;
    }
    if (!has_node_(from_id) || !has_node_(to_id)) {
        return false;
    }
    if (from_id == to_id) {
        return true;
    }

    const CookInvalidationClosureResult closure = transitive_successors(from_id);
    if (!closure.ok) {
        return false;
    }

    return std::find(closure.job_ids.begin(), closure.job_ids.end(), to_id) != closure.job_ids.end();
}

s32 CookDependencyGraph::topological_layer_index(const std::string& node_id) const {
    if (node_id.empty() || m_nodes.empty() || !has_node_(node_id)) {
        return -1;
    }

    const CookDependencyLayerResult layers = topological_layers();
    if (!layers.ok || layers.cycle_detected) {
        return -1;
    }

    for (std::size_t layer_index = 0; layer_index < layers.layers.size(); ++layer_index) {
        const std::vector<std::string>& layer = layers.layers[layer_index];
        if (std::find(layer.begin(), layer.end(), node_id) != layer.end()) {
            return static_cast<s32>(layer_index);
        }
    }

    return -1;
}

std::size_t CookDependencyGraph::parallel_layer_width() const {
    if (m_nodes.empty()) {
        return 0;
    }

    const CookDependencyLayerResult layers = topological_layers();
    if (!layers.ok || layers.cycle_detected || layers.layers.empty()) {
        return 0;
    }

    std::size_t width = 0;
    for (const std::vector<std::string>& layer : layers.layers) {
        width = std::max(width, layer.size());
    }
    return width;
}

std::vector<std::string> flatten_topological_layers(const CookDependencyLayerResult& layers) {
    if (!layers.ok || layers.cycle_detected || layers.layers.empty()) {
        return {};
    }

    std::vector<std::string> flattened;
    for (const std::vector<std::string>& layer : layers.layers) {
        flattened.insert(flattened.end(), layer.begin(), layer.end());
    }
    return flattened;
}

CookDependencyCycleResult CookDependencyGraph::detect_cycle_edges() const {
    CookDependencyCycleResult result;

    if (m_nodes.empty()) {
        return result;
    }

    const CookJobGraphOrderResult order = topological_order();
    if (!order.cycle_detected) {
        return result;
    }

    result.cycle_detected = true;

    std::unordered_map<std::string, u8> state;
    std::unordered_map<std::string, std::vector<std::string>> adjacency;
    for (const std::string& node : m_nodes) {
        state[node] = 0;
        adjacency[node] = {};
    }
    for (const CookJobDependencyEdge& edge : m_edges) {
        if (state.find(edge.from_job_id) == state.end() || state.find(edge.to_job_id) == state.end()) {
            continue;
        }
        adjacency[edge.from_job_id].push_back(edge.to_job_id);
    }

    std::vector<std::string> stack;
    const auto dfs = [&](const auto& self, const std::string& node) -> void {
        state[node] = 1;
        stack.push_back(node);

        for (const std::string& next : adjacency[node]) {
            if (state[next] == 0) {
                self(self, next);
            } else if (state[next] == 1) {
                result.cycle_edges.push_back({node, next});
            }
        }

        stack.pop_back();
        state[node] = 2;
    };

    for (const std::string& node : m_nodes) {
        if (state[node] == 0) {
            dfs(dfs, node);
        }
    }

    std::sort(result.cycle_edges.begin(), result.cycle_edges.end(),
              [](const CookJobDependencyEdge& left, const CookJobDependencyEdge& right) {
                  if (left.from_job_id != right.from_job_id) {
                      return left.from_job_id < right.from_job_id;
                  }
                  return left.to_job_id < right.to_job_id;
              });

    return result;
}

} // namespace fuse::project
