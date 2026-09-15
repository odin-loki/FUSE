#include <fuse/project/cook_dependency_graph.hpp>

#include <algorithm>
#include <unordered_map>

namespace fuse::project {

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

bool CookDependencyGraph::has_cycle() const {
    return topological_order().cycle_detected;
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
